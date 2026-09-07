// SPDX-License-Identifier: GPL-3.0-or-later

using Microsoft.UI.Dispatching;

namespace MediaPerch.Shell.Ipc;

/// <summary>
/// The one engine this process talks to, and what it last said.
/// </summary>
/// <remarks>
/// <para>
/// <b>One pipe, one tick, however many pages.</b> The window is pages now --
/// what is playing, the playlist, the shape the engine is in -- and each of
/// them wants the same status, the same playlist and the same connection. A
/// client per page would be several shells to the engine and several timers
/// asking it the same question, so there is one of each here and the pages
/// listen.
/// </para>
/// <para>
/// <b>It asks; nothing is pushed at it.</b> §10's engine has no idea a shell
/// exists, which is what lets it keep playing when one dies, so the asking is
/// this side's job. A second is the slowest a person would accept a transport
/// being wrong by and the cheapest thing that is: a few messages on a pipe
/// against a compositor already waking at the display's rate.
/// </para>
/// <para>
/// Every call may come back null and that is the ordinary case rather than an
/// error: the engine is a separate process that may not be running, and a
/// shell that cannot reach it draws that state.
/// </para>
/// </remarks>
internal sealed class Session
{
    /// <summary>The process's one session.</summary>
    public static Session Current { get; } = new();

    public EngineClient Engine { get; } = new();

    /// <summary>The last answer to <c>status</c>.</summary>
    public Status Status { get; private set; } = new();

    /// <summary>The last answer to <c>playlist</c>, and which entry is playing.</summary>
    public IReadOnlyList<string> Playlist { get; private set; } = Array.Empty<string>();

    public uint PlaylistAt { get; private set; }

    public bool Connected => Engine.Connected;

    /// <summary>Raised on the UI thread after the status or the playlist was read again.</summary>
    public event Action? Changed;

    /// <summary>Raised on the UI thread when the connection was made or lost.</summary>
    public event Action? ConnectionChanged;

    /// <summary>
    /// Raised on the UI thread once a second, after <see cref="Changed"/>, for
    /// the things that are not status: the picture asks whether it is still
    /// the same picture on this.
    /// </summary>
    public event Action? Tick;

    private DispatcherQueueTimer? _timer;
    private bool _ticking;
    private bool _wasConnected;

    /// <summary>Starts the tick on the UI thread's queue. Once.</summary>
    public void Start(DispatcherQueue queue)
    {
        if (_timer is not null)
        {
            return;
        }
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await OnTickAsync();
        _timer.Start();
        _ = ReconnectAsync();
    }

    private async Task OnTickAsync()
    {
        if (_ticking)
        {
            return; // a slow answer does not stack
        }
        _ticking = true;
        try
        {
            if (!Engine.Connected)
            {
                // Quietly, and briefly: a tick must not hang on a pipe that is
                // not there, and an engine that appears is noticed within a
                // second or two.
                await Engine.ConnectAsync(200, CancellationToken.None);
                Announce();
                if (!Engine.Connected)
                {
                    return;
                }
            }
            await RefreshAsync();
            Tick?.Invoke();
        }
        finally
        {
            _ticking = false;
        }
    }

    public async Task ReconnectAsync()
    {
        if (!Engine.Connected)
        {
            // A second is long enough for a service that is running and short
            // enough that a window does not sit blank while one is not.
            await Engine.ConnectAsync(1000, CancellationToken.None);
        }
        Announce();
        if (Engine.Connected)
        {
            await RefreshAsync();
        }
    }

    private void Announce()
    {
        if (Engine.Connected != _wasConnected)
        {
            _wasConnected = Engine.Connected;
            ConnectionChanged?.Invoke();
        }
    }

    /// <summary>Asks for the status and the playlist, and says so.</summary>
    public async Task RefreshAsync()
    {
        bool changed = await ReadStatusAsync();
        changed |= await ReadPlaylistAsync();
        if (changed)
        {
            Changed?.Invoke();
        }
    }

    private async Task<bool> ReadStatusAsync()
    {
        Answer? answer = await Engine.CallAsync(Kind.Status);
        if (answer is null || !answer.Value.Is(Kind.StatusReply))
        {
            Announce();
            return false;
        }
        Reader r = answer.Value.Reader();
        Status status = Decode.ReadStatus(r);
        // **`Complete` and not merely `Ok`.** A reply this build read most of
        // is a reply whose fields have moved.
        if (!r.Complete)
        {
            return false;
        }
        Status = status;
        return true;
    }

    private async Task<bool> ReadPlaylistAsync()
    {
        Answer? answer = await Engine.CallAsync(Kind.Playlist);
        if (answer is null || !answer.Value.Is(Kind.PlaylistReply))
        {
            return false;
        }
        Reader r = answer.Value.Reader();
        List<string> files = Decode.ReadStrings(r);
        uint at = r.U32();
        if (!r.Complete)
        {
            return false;
        }
        if (at == PlaylistAt && files.SequenceEqual(Playlist))
        {
            return false; // once a second, and almost always the same list
        }
        Playlist = files;
        PlaylistAt = at;
        return true;
    }

    /// <summary>
    /// Sends, and reports the engine's answer: empty when it was taken, and the
    /// engine's own sentence when it was not.
    /// </summary>
    public async Task<string> TakenAsync(Kind kind, Writer? payload = null)
    {
        Answer? answer = await Engine.CallAsync(kind, payload?.Bytes());
        if (answer is null)
        {
            return "The engine did not answer.";
        }
        return answer.Value.Is(Kind.Ok) ? string.Empty : ErrorText(answer.Value);
    }

    /// <summary>The sentence in an <c>error</c> frame, or a stand-in.</summary>
    public static string ErrorText(Answer answer)
    {
        if (!answer.Is(Kind.Error))
        {
            return $"The engine answered {answer.Header.Kind}.";
        }
        Reader r = answer.Reader();
        string why = r.Str();
        return r.Complete && why.Length != 0 ? why : "The engine refused it.";
    }

    /// <summary>A transport verb with no payload, followed by a fresh status.</summary>
    public async Task TransportAsync(Kind kind)
    {
        await Engine.CallAsync(kind);
        await RefreshAsync();
    }

    /// <summary>
    /// A seek, in frames of the source, relative or absolute -- the queue's
    /// coordinate either way, which is what the engine's seek speaks.
    /// </summary>
    public async Task SeekAsync(long frames, bool relative)
    {
        var payload = new Writer();
        payload.U8(relative ? (byte)1 : (byte)0);
        payload.I64(frames);
        await TakenAsync(Kind.Seek, payload);
        await RefreshAsync();
    }

    /// <summary>
    /// Plays the playlist from <paramref name="index"/>: the click on a track.
    /// The run starts again there, which is a real gap and is what a person
    /// who clicked asked for.
    /// </summary>
    public async Task<string> PlayAtAsync(uint index)
    {
        var payload = new Writer();
        payload.U32(index);
        string why = await TakenAsync(Kind.PlayAt, payload);
        await RefreshAsync();
        return why;
    }

    /// <summary>The file's own name; the path is what a tooltip is for.</summary>
    public static string Leaf(string path)
    {
        int cut = path.LastIndexOfAny(new[] { '\\', '/' });
        return cut < 0 ? path : path[(cut + 1)..];
    }

    /// <summary>Frames as <c>m:ss</c>.</summary>
    public static string Clock(ulong frames, uint rate)
    {
        ulong seconds = rate == 0 ? 0 : frames / rate;
        return $"{seconds / 60}:{seconds % 60:D2}";
    }
}

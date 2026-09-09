// SPDX-License-Identifier: GPL-3.0-or-later

using System.Diagnostics;
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
    /// <summary>
    /// Writes one line to <c>%TEMP%\mediaperch-shell.log</c> and to the error
    /// stream. A window has no console, and the failures worth writing down --
    /// an engine that would not start, an exception nothing observed -- are
    /// the ones a person cannot see on the window.
    /// </summary>
    public static string LogPath => Path.Combine(Path.GetTempPath(), "mediaperch-shell.log");

    public static void Log(string line)
    {
        try
        {
            File.AppendAllText(LogPath,
                               $"{DateTime.Now:HH:mm:ss.fff} {line}{Environment.NewLine}");
        }
        catch (IOException)
        {
            // A log that cannot be written is not worth a second failure.
        }
        Console.Error.WriteLine(line);
    }

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
    /// Whether the first connect -- which may be starting an engine -- is still
    /// under way, so the tick does not open a second pipe beside it: two
    /// connects landing at once would each keep a stream and one would be leaked.
    private bool _connecting;

    /// <summary>
    /// The engine this shell started, or null for one that was already there.
    /// </summary>
    /// <remarks>
    /// <b>The shell owns what it started, and nothing else.</b> An engine that
    /// was running before the window opened -- started from the CLI, or by a
    /// previous shell that is still playing -- is a service somebody else is
    /// using, and closing a window is not a reason to stop their music. One
    /// this window brought up to have something to talk to goes when the
    /// window goes, asked politely first.
    /// </remarks>
    private Process? _started;

    /// <summary>
    /// When this shell started the engine again after it died, for the rule
    /// that it does so at most three times in a minute: an engine that dies
    /// at every start is a fault to read the log about, not a loop to sit in.
    /// </summary>
    private readonly List<DateTime> _restarts = new();

    /// <summary>What the last attempt to find or start an engine said, for the title bar.</summary>
    public string EngineNote { get; private set; } = string.Empty;

    /// <summary>Whether the engine that is (or was) running is one this shell started.</summary>
    public bool StartedHere => _started is not null;

    /// <summary>
    /// Connects, and when nothing is listening, starts <c>mediaperchd</c> from
    /// beside this executable and connects to that.
    /// </summary>
    /// <remarks>
    /// Beside, because that is where it ships (§10: an install is the engine,
    /// the CLI and, optionally, this) and because a shell that went looking
    /// anywhere else would be a shell that started the wrong engine.
    /// <c>MEDIAPERCH_ENGINE</c> names another one, for a machine that keeps
    /// them apart.
    /// </remarks>
    public async Task EnsureEngineAsync()
    {
        if (Engine.Connected)
        {
            EngineNote = string.Empty;
            return;
        }
        if (await Engine.ConnectAsync(1000, CancellationToken.None))
        {
            // Somebody else's engine -- a terminal's, or a shell's that is
            // still running -- and not necessarily the build this shell was
            // built beside. Said, because *which mediaperchd is this* is the
            // first question when the engine does something unexpected.
            Log("engine: one was already listening; this shell did not start it");
            EngineNote = string.Empty;
            return;
        }
        string path = EnginePath();
        if (!File.Exists(path))
        {
            EngineNote = $"no engine: {path} is not there";
            return;
        }
        Log($"engine: starting {path} (built {File.GetLastWriteTime(path):yyyy-MM-dd HH:mm:ss})");
        try
        {
            _started = Process.Start(new ProcessStartInfo(path)
            {
                // Its own console would be a second window; its tray icon is
                // for an engine with no shell, and this one has one.
                Arguments = "--no-tray",
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(path) ?? AppContext.BaseDirectory,
            });
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            EngineNote = $"the engine would not start: {e.Message}";
            return;
        }
        // It opens its pipe within a moment of starting; a machine under load
        // gets a few seconds before this gives up on it.
        for (int i = 0; i < 50 && _started is { HasExited: false }; ++i)
        {
            if (await Engine.ConnectAsync(100, CancellationToken.None))
            {
                EngineNote = string.Empty;
                return;
            }
        }
        EngineNote = _started is { HasExited: true }
            ? $"the engine started and exited with code {_started.ExitCode}"
            : "the engine started but is not answering on its pipe";
    }

    /// <summary>
    /// Where <c>mediaperchd.exe</c> is: beside this executable, which is where
    /// an install puts it; or where <c>MEDIAPERCH_ENGINE</c> says; or where the
    /// file <c>mediaperch.engine</c> beside this executable says, which the
    /// tree's own build writes and an install does not ship.
    /// </summary>
    private static string EnginePath()
    {
        string beside = Path.Combine(AppContext.BaseDirectory, "mediaperchd.exe");
        if (File.Exists(beside))
        {
            return beside;
        }
        string? named = Environment.GetEnvironmentVariable("MEDIAPERCH_ENGINE");
        if (!string.IsNullOrWhiteSpace(named))
        {
            return named;
        }
        string pointer = Path.Combine(AppContext.BaseDirectory, "mediaperch.engine");
        if (File.Exists(pointer))
        {
            try
            {
                string written = File.ReadAllText(pointer).Trim();
                if (written.Length != 0)
                {
                    return written;
                }
            }
            catch (IOException)
            {
                // Then it is as good as absent.
            }
        }
        return beside;
    }

    /// <summary>
    /// Asks the engine this shell started to quit, and waits for it; one that
    /// will not go in a couple of seconds is stopped. An engine that was
    /// already running is left running.
    /// </summary>
    public void Stop()
    {
        _timer?.Stop();
        if (_started is null || _started.HasExited)
        {
            return;
        }
        try
        {
            // Off the UI thread, and bounded: a window closing must not hang
            // on a pipe.
            Task.Run(() => Engine.CallAsync(Kind.Quit)).Wait(1000);
            if (!_started.WaitForExit(2000))
            {
                _started.Kill();
            }
        }
        catch (Exception e) when (e is AggregateException or InvalidOperationException
                                         or System.ComponentModel.Win32Exception)
        {
            // Already gone, or would not be asked; either way there is nothing
            // left to stop.
            _ = e;
        }
    }

    /// <summary>
    /// Stops the engine this shell started and starts it again -- what an
    /// engine setting that takes effect at the next start asks for. An engine
    /// this shell did not start is somebody else's and is left alone, and the
    /// answer says so.
    /// </summary>
    /// <returns>Empty when the engine came back, else why not.</returns>
    public async Task<string> RestartEngineAsync()
    {
        if (_started is null)
        {
            return "this shell did not start the engine that is running, so it will not "
                   + "restart it; stop it yourself and press Reconnect";
        }
        Log("engine: restarting at a person's request");
        if (!_started.HasExited)
        {
            try
            {
                await Engine.CallAsync(Kind.Quit).WaitAsync(TimeSpan.FromSeconds(1));
            }
            catch (TimeoutException)
            {
                // Then it is stopped below.
            }
            if (!_started.WaitForExit(2000))
            {
                try
                {
                    _started.Kill();
                }
                catch (Exception e) when (e is InvalidOperationException
                                                 or System.ComponentModel.Win32Exception)
                {
                    // Already gone.
                    _ = e;
                }
            }
        }
        Engine.Close();
        _started = null;
        _restarts.Clear();
        Announce();
        await EnsureEngineAsync();
        Announce();
        if (Engine.Connected)
        {
            await RefreshAsync();
            return string.Empty;
        }
        return EngineNote.Length == 0 ? "the engine did not come back" : EngineNote;
    }

    /// <summary>
    /// An engine this shell started and that has exited is started again --
    /// at most three times in a minute -- and the title bar says what happened
    /// meanwhile. The picture re-attaches by itself: the new engine's surface
    /// is a new generation.
    /// </summary>
    private async Task RestartIfDeadAsync()
    {
        if (_started is not { HasExited: true } || Engine.Connected)
        {
            return;
        }
        int code = _started.ExitCode;
        _started = null;
        Engine.Close();
        Log($"engine: the engine this shell started exited with code {code}");
        DateTime now = DateTime.UtcNow;
        _restarts.RemoveAll(then => now - then > TimeSpan.FromMinutes(1));
        if (_restarts.Count >= 3)
        {
            EngineNote = $"the engine exited with code {code} three times in a minute; not "
                         + $"starting it again -- see {LogPath} and the engine's own log";
            ConnectionChanged?.Invoke();
            return;
        }
        _restarts.Add(now);
        EngineNote = $"the engine exited with code {code}; starting it again";
        ConnectionChanged?.Invoke();
        await EnsureEngineAsync();
        if (Engine.Connected)
        {
            EngineNote = $"the engine exited with code {code} and was started again";
        }
        ConnectionChanged?.Invoke();
    }

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
        _ = FirstConnectAsync();
    }

    private async Task FirstConnectAsync()
    {
        _connecting = true;
        try
        {
            await EnsureEngineAsync();
        }
        catch (Exception e)
        {
            // **Everything, because this is the edge of a fire-and-forget.**
            // An exception past here has nobody to observe it, and a shell that
            // silently never connected is the worst version of this failure.
            EngineNote = "the engine could not be reached: " + e.Message;
            Log("engine: " + e);
        }
        finally
        {
            _connecting = false;
        }
        Announce();
        if (Engine.Connected)
        {
            await RefreshAsync();
            return;
        }
        // **On the error stream as well as in the title bar.** Somebody running
        // this from a terminal to find out why there is no engine gets the
        // sentence there, which is where they are looking.
        Log("engine: " + (EngineNote.Length == 0 ? "not connected, and no reason recorded"
                                                  : EngineNote));
        ConnectionChanged?.Invoke(); // so the note is drawn even when nothing changed
    }

    private async Task OnTickAsync()
    {
        if (_ticking || _connecting)
        {
            return; // a slow answer does not stack, and a first connect is not raced
        }
        _ticking = true;
        try
        {
            await RestartIfDeadAsync();
            if (!Engine.Connected)
            {
                // What ended the last call, if the client knows: an answer
                // that never came is worth a sentence in the title bar.
                if (Engine.Trouble.Length != 0)
                {
                    EngineNote = Engine.Trouble;
                }
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
            if (Engine.Connected)
            {
                EngineNote = string.Empty;
            }
            else if (EngineNote.Length == 0 && Engine.Trouble.Length != 0)
            {
                EngineNote = Engine.Trouble;
            }
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

    /// <summary>
    /// Plays <paramref name="files"/> -- replacing the playlist, or appended to
    /// it -- and answers why not, or empty.
    /// </summary>
    public async Task<string> PlayFilesAsync(IReadOnlyList<string> files, bool replace)
    {
        if (files.Count == 0)
        {
            return string.Empty;
        }
        var payload = new Writer();
        payload.Strings(files);
        string why = await TakenAsync(replace ? Kind.Play : Kind.Enqueue, payload);
        await RefreshAsync();
        return why;
    }

    /// <summary>
    /// Moves playlist entry <paramref name="from"/> to <paramref name="to"/>.
    /// The engine refuses an entry its queue has already reached, in a
    /// sentence that says how far that is.
    /// </summary>
    public async Task<string> MoveAsync(uint from, uint to)
    {
        var payload = new Writer();
        payload.U32(from);
        payload.U32(to);
        string why = await TakenAsync(Kind.MoveEntry, payload);
        await RefreshAsync();
        return why;
    }

    public async Task<string> ClearAsync()
    {
        string why = await TakenAsync(Kind.Clear);
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

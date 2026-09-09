// SPDX-License-Identifier: GPL-3.0-or-later

using System.IO.Pipes;

namespace MediaPerch.Shell.Ipc;

/// <summary>What came back: the header and whatever followed it.</summary>
internal readonly record struct Answer(Header Header, byte[] Body)
{
    public Reader Reader() => new(Body);
    public bool Is(Kind kind) => Header.Kind == kind;
}

/// <summary>
/// The engine, over §10's named pipe.
/// </summary>
/// <remarks>
/// <para>
/// <b>The shell is killable at any moment and playback does not notice.</b>
/// That is the whole reason for the split, and it shapes this class: nothing
/// here holds anything the engine needs, every call may fail because the engine
/// is not running, and a dropped connection is a state to draw rather than an
/// exception to escape.
/// </para>
/// <para>
/// One request at a time on one pipe, with an id echoed in the reply. The
/// engine may send an event between a request and its answer -- that is what
/// <c>subscribe</c> is for -- so a reply is matched by id and anything else with
/// id zero is an event.
/// </para>
/// </remarks>
internal sealed class EngineClient : IDisposable
{
    private readonly string _pipe;
    private NamedPipeClientStream? _stream;
    private uint _nextId = 1;
    private readonly SemaphoreSlim _one = new(1, 1);

    /// <summary>
    /// How long one answer may take. **Bounded, because a shell that waits
    /// for ever on a pipe is a shell that has to be killed.** The engine
    /// answers a setting under its display loop's hold with a deadline of
    /// its own (1.5 s for a size), so five seconds is an engine that is not
    /// answering at all, and the connection is dropped so that the tick
    /// notices and says so.
    /// </summary>
    private static readonly TimeSpan AnswerWithin = TimeSpan.FromSeconds(5);

    /// <summary>Why the last call ended without an answer, or empty.</summary>
    public string Trouble { get; private set; } = string.Empty;

    public EngineClient(string pipe = Protocol.DefaultPipe)
    {
        _pipe = pipe;
    }

    public bool Connected => _stream is { IsConnected: true };

    /// <summary>Events the engine sent unasked, in the order they arrived.</summary>
    public event Action<Answer>? Event;

    public async Task<bool> ConnectAsync(int millisecondsTimeout, CancellationToken token)
    {
        Close();
        var stream = new NamedPipeClientStream(".", _pipe, PipeDirection.InOut,
                                               PipeOptions.Asynchronous);
        try
        {
            await stream.ConnectAsync(millisecondsTimeout, token).ConfigureAwait(false);
        }
        catch (Exception)
        {
            // **Not running is the ordinary case**, not an error: the shell is
            // optional and the engine may simply not be up yet. Whoever asked
            // draws that state.
            stream.Dispose();
            return false;
        }
        _stream = stream;
        Trouble = string.Empty;
        return true;
    }

    public void Close()
    {
        _stream?.Dispose();
        _stream = null;
    }

    public void Dispose()
    {
        Close();
        _one.Dispose();
    }

    /// <summary>
    /// One request, and the reply that carries its id.
    /// </summary>
    /// <remarks>
    /// Serialised on one semaphore because there is one pipe: two requests
    /// interleaved on it would be two half-messages. Events that arrive in the
    /// meantime are raised and skipped over rather than dropped.
    /// </remarks>
    public async Task<Answer?> CallAsync(Kind kind, byte[]? payload = null,
                                         CancellationToken token = default)
    {
        NamedPipeClientStream? stream = _stream;
        if (stream is not { IsConnected: true })
        {
            return null;
        }

        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(token);
        bounded.CancelAfter(AnswerWithin);
        try
        {
            await _one.WaitAsync(bounded.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Another call has held the pipe for the whole of the allowance:
            // that one will report, and this one simply did not happen.
            return null;
        }
        try
        {
            uint id = _nextId++;
            if (_nextId == 0)
            {
                _nextId = 1; // zero means an event, so it is never a request's
            }
            await WriteFrameAsync(stream, kind, id, payload, bounded.Token).ConfigureAwait(false);

            for (;;)
            {
                Answer? answer = await ReadFrameAsync(stream, bounded.Token).ConfigureAwait(false);
                if (answer is null)
                {
                    Close();
                    return null;
                }
                if (answer.Value.Header.Id == 0)
                {
                    Event?.Invoke(answer.Value);
                    continue;
                }
                return answer.Value.Header.Id == id ? answer : null;
            }
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        {
            Trouble = $"the engine did not answer {kind} within {AnswerWithin.TotalSeconds:0} s";
            Close();
            return null;
        }
        catch (Exception)
        {
            Close();
            return null;
        }
        finally
        {
            _one.Release();
        }
    }

    /// <summary>
    /// Waits for whatever the engine says next. What a subscribed shell runs in
    /// a loop of its own.
    /// </summary>
    public async Task<Answer?> ReadAsync(CancellationToken token)
    {
        NamedPipeClientStream? stream = _stream;
        if (stream is not { IsConnected: true })
        {
            return null;
        }
        await _one.WaitAsync(token).ConfigureAwait(false);
        try
        {
            return await ReadFrameAsync(stream, token).ConfigureAwait(false);
        }
        catch (Exception)
        {
            Close();
            return null;
        }
        finally
        {
            _one.Release();
        }
    }

    private static async Task WriteFrameAsync(Stream stream, Kind kind, uint id,
                                              byte[]? payload, CancellationToken token)
    {
        var head = new Writer();
        head.U32(Protocol.Magic);
        // Version and kind are sixteen bits each, byte at a time and in the
        // order the header declares them -- which is what `frame` does and is
        // why this is not a struct copy.
        head.U8((byte)(Protocol.Version & 0xFF));
        head.U8((byte)((Protocol.Version >> 8) & 0xFF));
        head.U8((byte)((ushort)kind & 0xFF));
        head.U8((byte)(((ushort)kind >> 8) & 0xFF));
        head.U32(id);
        head.U32((uint)(payload?.Length ?? 0));

        byte[] bytes = head.Bytes();
        await stream.WriteAsync(bytes, token).ConfigureAwait(false);
        if (payload is { Length: > 0 })
        {
            await stream.WriteAsync(payload, token).ConfigureAwait(false);
        }
        await stream.FlushAsync(token).ConfigureAwait(false);
    }

    private static async Task<Answer?> ReadFrameAsync(Stream stream, CancellationToken token)
    {
        byte[] head = new byte[Protocol.HeaderBytes];
        if (!await FillAsync(stream, head, token).ConfigureAwait(false))
        {
            return null;
        }
        var r = new Reader(head);
        uint magic = r.U32();
        ushort version = (ushort)(r.U8() | (r.U8() << 8));
        var kind = (Kind)(ushort)(r.U8() | (r.U8() << 8));
        uint id = r.U32();
        uint payload = r.U32();
        // **A header that is not one is a connection to drop**, not a message to
        // skip: the stream is no longer where this thinks it is.
        if (magic != Protocol.Magic || version != Protocol.Version ||
            payload > Protocol.MaxPayload)
        {
            return null;
        }

        byte[] body = new byte[payload];
        if (payload != 0 && !await FillAsync(stream, body, token).ConfigureAwait(false))
        {
            return null;
        }
        return new Answer(new Header(magic, version, kind, id, payload), body);
    }

    private static async Task<bool> FillAsync(Stream stream, byte[] into,
                                              CancellationToken token)
    {
        int at = 0;
        while (at < into.Length)
        {
            int got = await stream.ReadAsync(into.AsMemory(at), token).ConfigureAwait(false);
            if (got <= 0)
            {
                return false;
            }
            at += got;
        }
        return true;
    }
}

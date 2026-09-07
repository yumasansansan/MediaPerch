// SPDX-License-Identifier: GPL-3.0-or-later

namespace MediaPerch.Shell.Ipc;

/// <summary>
/// The other half of <c>src/player/mediaperch/protocol.hpp</c>.
/// </summary>
/// <remarks>
/// <b>Two descriptions of one wire, and that is a cost this takes on
/// deliberately.</b> The alternative was a serialiser or a generator, and §10's
/// framing is small enough that either would be more machinery than the thing
/// it describes. What keeps them in step is that the format is versioned and
/// the engine answers <c>error</c> to a kind it does not know -- so a shell that
/// is ahead of its engine is told, rather than reading a field that moved.
/// </remarks>
public static class Protocol
{
    public const uint Magic = 0x5049504Du;
    public const ushort Version = 1;
    public const uint MaxPayload = 1u << 20;
    public const int HeaderBytes = 16;

    /// <summary>The engine's default pipe, matching <c>default_pipe_name</c>.</summary>
    public const string DefaultPipe = @"mediaperch";
}

/// <summary>
/// What a message is. Numbered in three ranges so that reading a trace tells
/// you which direction it was going without a table.
/// </summary>
public enum Kind : ushort
{
    // requests, shell to engine
    Hello = 1,
    Status = 2,
    Play = 3,
    Enqueue = 4,
    Clear = 5,
    Pause = 6,
    Resume = 7,
    Stop = 8,
    Seek = 9,
    Next = 10,
    Previous = 11,
    Playlist = 12,
    Settings = 13,
    SettingSet = 14,
    Log = 15,
    Subscribe = 16,
    Save = 17,
    Quit = 18,
    Calibrate = 19,
    Profile = 20,
    Display = 21,
    Graph = 22,
    NodeSettings = 23,
    NodeSettingSet = 24,
    Modules = 25,
    EngineSettings = 26,
    EngineSettingSet = 27,
    Surface = 28,
    PlayAt = 29,

    // replies, engine to shell
    Ok = 128,
    Error = 129,
    HelloReply = 130,
    StatusReply = 131,
    PlaylistReply = 132,
    SettingsReply = 133,
    LogReply = 134,
    ProfileReply = 135,
    GraphReply = 136,
    NodeSettingsReply = 137,
    ModulesReply = 138,
    EngineSettingsReply = 139,
    SurfaceReply = 140,

    // events, engine to shell, unasked
    EventState = 200,
    EventLog = 201,
}

public enum PlayState : uint
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
}

/// <summary>What a node is. Numbers rather than words, as the wire has them.</summary>
public enum NodeKind : uint
{
    Source = 0,
    Convert = 1,
    Dsp = 2,
    Sink = 3,
    VideoSource = 4,
    VideoStage = 5,
    Presenter = 6,
}

[Flags]
public enum NodeFlags : uint
{
    None = 0,
    Removable = 1 << 0,
    Settable = 1 << 1,
}

public readonly record struct Header(uint Magic, ushort Version, Kind Kind, uint Id,
                                       uint Payload);

public sealed record Node(string Id, NodeKind Kind, string Module, string Name,
                            NodeFlags Flags);

public sealed record Edge(string From, string To);

public sealed class Graph
{
    public List<Node> Nodes { get; } = new();
    public List<Edge> Edges { get; } = new();
}

/// <summary>One row of a module's own settings.</summary>
/// <param name="ReadOnly">
/// A measurement rather than a setting -- a peak, a cost, a latency, a handle.
/// The engine decides this, from the <c>(read only)</c> every module's
/// <c>describe</c> ends such a row with; a shell that decided by looking for
/// those two words would be parsing English over a wire.
/// </param>
public sealed record Setting(string Key, string Value, string Description,
                               bool ReadOnly);

public sealed record ModuleRow(uint Kind, string Id, string Name, uint Priority,
                                 bool Allowed);

public sealed record Format(uint SampleRate, uint Channels, uint ChannelMask,
                              uint SampleType, uint Encoding, uint ValidBits);

public sealed class Status
{
    public PlayState State { get; init; }
    public uint Index { get; init; }
    public uint Count { get; init; }
    public ulong Position { get; init; }
    public ulong Length { get; init; }
    /// <summary>
    /// How far into this track, where <see cref="Position"/> is how far into
    /// the queue. The two are in different units: a gapless queue is one stream
    /// to the device, so the position counts straight through boundaries.
    /// </summary>
    public ulong ItemPosition { get; init; }
    public string Track { get; init; } = string.Empty;
    public string Decoder { get; init; } = string.Empty;
    public string Device { get; init; } = string.Empty;
    public Format Source { get; init; } = new(0, 0, 0, 0, 0, 0);
    public Format Wire { get; init; } = new(0, 0, 0, 0, 0, 0);
    public uint Fidelity { get; init; }
    public bool Processed { get; init; }
    public ulong FramesRendered { get; init; }
    public ulong Underruns { get; init; }
    public string Error { get; init; } = string.Empty;
}

/// <summary>Decoding, kept beside the types it decodes.</summary>
public static class Decode
{
    public static Format ReadFormat(Reader r) =>
        new(r.U32(), r.U32(), r.U32(), r.U32(), r.U32(), r.U32());

    public static Status ReadStatus(Reader r)
    {
        var state = (PlayState)r.U32();
        uint index = r.U32();
        uint count = r.U32();
        ulong position = r.U64();
        ulong length = r.U64();
        ulong itemPosition = r.U64();
        string track = r.Str();
        string decoder = r.Str();
        string device = r.Str();
        Format source = ReadFormat(r);
        Format wire = ReadFormat(r);
        uint fidelity = r.U32();
        bool processed = r.U8() != 0;
        ulong rendered = r.U64();
        ulong underruns = r.U64();
        string error = r.Str();
        return new Status
        {
            State = state,
            Index = index,
            Count = count,
            Position = position,
            Length = length,
            ItemPosition = itemPosition,
            Track = track,
            Decoder = decoder,
            Device = device,
            Source = source,
            Wire = wire,
            Fidelity = fidelity,
            Processed = processed,
            FramesRendered = rendered,
            Underruns = underruns,
            Error = error,
        };
    }

    public static List<Setting> ReadSettings(Reader r)
    {
        var out_ = new List<Setting>();
        uint n = r.U32();
        if (!r.Ok || n > 1u << 16)
        {
            return out_;
        }
        for (uint i = 0; i < n; ++i)
        {
            out_.Add(new Setting(r.Str(), r.Str(), r.Str(), r.U8() != 0));
        }
        return out_;
    }

    public static List<string> ReadStrings(Reader r)
    {
        var out_ = new List<string>();
        uint n = r.U32();
        if (!r.Ok || n > 1u << 16)
        {
            return out_;
        }
        for (uint i = 0; i < n; ++i)
        {
            out_.Add(r.Str());
        }
        return out_;
    }

    public static Graph ReadGraph(Reader r)
    {
        var graph = new Graph();
        uint nodes = r.U32();
        if (!r.Ok || nodes > 1u << 16)
        {
            return graph;
        }
        for (uint i = 0; i < nodes; ++i)
        {
            string id = r.Str();
            var kind = (NodeKind)r.U32();
            string module = r.Str();
            string name = r.Str();
            var flags = (NodeFlags)r.U32();
            graph.Nodes.Add(new Node(id, kind, module, name, flags));
        }
        uint edges = r.U32();
        if (!r.Ok || edges > 1u << 16)
        {
            return graph;
        }
        for (uint i = 0; i < edges; ++i)
        {
            graph.Edges.Add(new Edge(r.Str(), r.Str()));
        }
        return graph;
    }

    public static List<ModuleRow> ReadModules(Reader r)
    {
        var out_ = new List<ModuleRow>();
        uint n = r.U32();
        if (!r.Ok || n > 1u << 16)
        {
            return out_;
        }
        for (uint i = 0; i < n; ++i)
        {
            out_.Add(new ModuleRow(r.U32(), r.Str(), r.Str(), r.U32(), r.U8() != 0));
        }
        return out_;
    }
}

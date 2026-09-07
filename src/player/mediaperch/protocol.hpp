// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the engine and a shell say to each other.
//
// **A versioned binary stream, not a text protocol.** The reason is the same
// one that made the module ABI plain C: a protocol somebody has to parse is a
// protocol somebody parses differently, and two processes disagreeing about
// what "position" means is a bug nobody can see. Every field here has a width,
// an order and an endianness, and the version in the header says which set of
// them a message belongs to.
//
// **The shell is not trusted.** It is a separate process, it may be a third
// party's, and it may be malicious or simply wrong. So the reader never throws,
// never reads past the end, and never allocates on a length it has not checked:
// a truncated message and a hostile one take the same path, which is to be
// rejected. The engine's answer to a message it cannot parse is to close the
// connection, not to guess.
//
// **The surface is small on purpose.** Transport, the playlist, the settings
// and a log tail. A shell cannot reach into the graph, cannot hand the engine a
// buffer, and cannot make it do anything the engine would not do on its own --
// which is what makes a third-party shell an ordinary thing to write rather
// than a fork.

#ifndef MEDIAPERCH_PROTOCOL_HPP
#define MEDIAPERCH_PROTOCOL_HPP

#include "mediaperch/format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mp::ipc {

/// "MPIP". First on the wire, so a client that connected to the wrong pipe
/// finds out immediately rather than after a plausible-looking length.
inline constexpr std::uint32_t k_magic = 0x5049504Du;

/// Bumped when a field changes meaning, moves, or goes away. Adding a *message*
/// does not need it: an engine that does not know a kind answers `error`, which
/// is exactly what a shell from the future should be told.
inline constexpr std::uint16_t k_version = 1;

/// Beyond this a length is a mistake or an attack, never a message. The largest
/// honest payload is a playlist, and a megabyte is some thousands of paths.
inline constexpr std::uint32_t k_max_payload = 1u << 20;

/// Every message begins with one of these, and it is always 16 bytes.
struct Header {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t kind;
    /// Echoed in the reply, so a shell may have several requests outstanding.
    /// Zero on an event, which answers nothing.
    std::uint32_t id;
    std::uint32_t payload;
};
inline constexpr std::size_t k_header_bytes = 16;

/// What a message is.
///
/// Numbered in three ranges rather than one, so that reading a trace tells you
/// which direction a message was going without a table.
enum class Kind : std::uint16_t {
    // --- requests, shell to engine ---
    hello = 1,
    status = 2,
    /// Replace the playlist with these paths and start at the first.
    play = 3,
    /// Add to the end of the playlist, without disturbing what is playing.
    enqueue = 4,
    clear = 5,
    pause = 6,
    resume = 7,
    stop = 8,
    seek = 9,
    next = 10,
    previous = 11,
    playlist = 12,
    settings = 13,
    setting_set = 14,
    log = 15,
    /// Ask for events. Until this arrives a connection is answers only, which
    /// is what a one-shot `mediaperch-cli status` wants.
    subscribe = 16,
    quit = 17,
    /// Write the settings file. Adding a kind does not need a version bump: an
    /// engine that does not know one answers `error`, which is exactly what a
    /// shell from the future should be told.
    save = 18,
    /// **Measure this machine against these files and write it down.**
    ///
    /// A calibration is playback of a list with a report at the end, so it
    /// arrives as one verb and takes nothing else with it: progress is
    /// `event_log`, because the driver's own progress lines are log lines and a
    /// shell that subscribed already shows them, and what is playing is
    /// `Status`, because during a calibration something *is* playing. §10's
    /// surface does not widen to hold this.
    calibrate = 19,
    /// What was measured, as the text of the profile. A shell displays it and
    /// does not parse it: what a measurement means is the core's, which is the
    /// same split §11 makes for the settings file.
    profile = 20,
    /// **Which display the picture is on, and what it is** (§9.4, §9.7.1).
    ///
    /// The one thing a windowless engine cannot work out for itself: given no
    /// window a presenter falls back to the first output, and for an engine
    /// that is a guess about which monitor the picture is on. Every §9 decision
    /// turns on it -- the tone mapper, the SDR boost, the HLG system gamma, the
    /// encoding -- so the shell says, and says it again whenever its window
    /// crosses a monitor or somebody toggles HDR.
    ///
    /// **A message and not a setting**, which is the distinction §11 draws: the
    /// keys under `[player]` are things a person chose and a file remembers,
    /// and where a window happens to be is neither. A saved one would be a
    /// stale one, replayed at the next startup about a monitor that may be gone.
    ///
    /// `u8 known, u8 hdr, u8 wide, f64 white_nits, f64 peak_nits`. `known` zero
    /// is *work it out yourself*, which is what a shell sends when it stops
    /// knowing -- minimised, or moved to a display it cannot describe.
    display = 21,
    /// **The shape the engine is in**, for a shell that draws it (§10's node
    /// canvas). Nodes and what connects them, derived from the graph that is
    /// actually built rather than from a model kept beside it -- two models of
    /// one graph are two things to keep in step, and the one that drifts is the
    /// one nobody plays through.
    graph = 22,
    /// One node's own settings, keyed by the id `graph` gave it. The settings
    /// button on a node.
    node_settings = 23,
    /// And changing one. A key a node does not have comes back as `error`.
    node_setting_set = 24,
    /// **Every module that could be a node**: its kind, its id, its priority
    /// and whether the allow-list admits it. The canvas's palette.
    modules = 25,
    /// **§11's `[engine]` half**: where to listen, where the modules are, which
    /// of them may load, which readers to prefer, and where the profile is.
    ///
    /// A separate verb from `settings` because they are a different kind of
    /// thing, and §11 drew that line for a reason: the keys under `[player]` are
    /// arguments to `Player::set` and change what is playing *now*, while these
    /// are read before there is a player and take effect at the next start. A
    /// shell that offered them in one list would be offering a knob that does
    /// nothing until a restart beside one that does something immediately.
    ///
    /// **The canvas needs them** all the same: module priority is `decoders`
    /// and the allow-list is `allow`, and a person reordering a palette is
    /// editing exactly those.
    engine_settings = 26,
    engine_setting_set = 27,
    /// **The picture, as a handle a shell can use** (§9.7.1).
    ///
    /// The engine draws into a composition surface it made and never shows; a
    /// shell puts that surface in a visual and commits, and from then on the
    /// frames it composites are frames that crossed no boundary. This is the
    /// one message that gets the handle across.
    ///
    /// **The shell sends its own process id and the engine duplicates.** A
    /// `HANDLE` is a number in one process and nothing in another, so the value
    /// has to be made valid on the far side by somebody -- and the engine is
    /// the side that should decide who gets one. A shell that asked for a
    /// surface and was refused is a shell with no picture, which is a state it
    /// has to be able to draw anyway.
    ///
    /// The reply is `u64`, and zero means there is nothing to show: no picture
    /// in the current track, or a presenter that draws into a window instead.
    surface = 28,
    /// `u32` index into the playlist. **The click on a track.** The run starts
    /// again there, which is a real gap in exclusive mode -- the device stops,
    /// the ring refills, the device starts -- and is what a person who clicked
    /// asked for. It is not a seek, because a queue records where a track began
    /// as it goes past it and cannot place one it has not reached; and it is
    /// not a string of `next`s, because each of those opens a file and none of
    /// them is atomic. Answered with `ok`, or `error` for an index the playlist
    /// does not have.
    play_at = 29,

    // --- replies, engine to shell ---
    ok = 128,
    error = 129,
    hello_reply = 130,
    status_reply = 131,
    playlist_reply = 132,
    settings_reply = 133,
    log_reply = 134,
    profile_reply = 135,
    graph_reply = 136,
    node_settings_reply = 137,
    modules_reply = 138,
    engine_settings_reply = 139,
    /// `u64` handle, then `u64` generation. See `Kind::surface`.
    surface_reply = 140,

    // --- events, engine to shell, unasked ---
    event_state = 200,
    event_log = 201,
};

/// **What to measure, and how much of somebody's afternoon to spend on it.**
///
/// Every field is here because every field costs real time: a calibration
/// cannot run faster than the material, so a shell that offered no choice would
/// be a shell that spent an hour without asking. The engine turns this into
/// `mp::CalibrationPlan`, which is where the meanings are; this is only the
/// wire.
struct Calibration {
    std::vector<std::string> files;
    /// `mp::Dimension` and `mp::Sweep` as integers. Named on the wire by their
    /// numbers rather than their words, because a word is a second spelling to
    /// keep in step and the core already has the first.
    std::uint32_t dimensions = 1;
    std::uint32_t sweep = 0;
    std::uint32_t windows = 3;
    double window_seconds = 10.0;
    std::uint32_t start_ring = 0;
    std::uint32_t lowest_ring = 1;
    std::uint32_t highest_ring = 8192;
};

/// What the engine is doing.
enum class State : std::uint32_t {
    stopped = 0,
    playing = 1,
    paused = 2,
};

[[nodiscard]] const char* state_name(State s) noexcept;
[[nodiscard]] const char* kind_name(Kind k) noexcept;

/// Everything a shell needs to draw a transport bar, in one message.
///
/// One message rather than a dozen getters because it is also the event: a
/// shell that subscribed is sent this whenever it changes, and a shell that
/// asked is sent the same bytes. Two encoders for one thing is how the two
/// drift apart.
struct Status {
    State state = State::stopped;
    /// Which playlist item, and how far into the queue in its own frames.
    std::uint32_t index = 0;
    std::uint32_t count = 0;
    std::uint64_t position = 0;
    /// The current item's length, or zero when nobody knows -- a stream, or a
    /// decoder that will not say.
    std::uint64_t length = 0;
    /// How far into **this track**, where `position` is how far into the
    /// queue.
    ///
    /// **The two are in different units and that was a bug for as long as
    /// nobody drew them together.** A gapless queue is one stream to the
    /// device, so `position` counts straight through every boundary -- it has
    /// to, because that is the coordinate a seek speaks -- while `length` is
    /// the track's. `mediaperch-cli status` printed them as `x / y` and said
    /// `0:14 / 0:01` on the sixteenth one-second track. Only the queue can
    /// convert, because it records where each track began as it goes past.
    std::uint64_t item_position = 0;
    std::string track;
    std::string decoder;
    std::string device;
    /// What the file is, and what the device agreed to. They are the same
    /// format on the path this program exists for, and saying both is how a
    /// person can see that they are.
    Format source{};
    Format wire{};
    /// `Fidelity` as an integer, and whether Path B is running.
    std::uint32_t fidelity = 0;
    bool processed = false;
    std::uint64_t frames_rendered = 0;
    std::uint64_t underruns = 0;
    /// Empty unless something went wrong, in which case it is in the engine's
    /// own words rather than a code the shell has to have a table for.
    std::string error;
};

/// One row of the settings tree: what it is called, what it is set to, and what
/// it means. The same shape `MpDspVtbl::describe` uses, for the same reason.
struct Setting {
    std::string key;
    std::string value;
    std::string description;
    /// **A measurement rather than a setting.** `peak`, `cost`, `latency`,
    /// `built`, the surface handle: rows a module answers with and will not
    /// take back.
    ///
    /// It has always been in the text -- every `describe` in this tree ends
    /// such a row's description with `(read only)` -- and that was fine while
    /// the only reader was a person. A shell has to decide whether to draw a
    /// box, and a shell that decided by looking for those two words would be
    /// parsing English over a wire. So it is read once, where the rows are
    /// parsed, and crosses as what it is.
    bool read_only = false;
};

/// What a node is, on the wire. **Numbers rather than words**, for the reason
/// `Dimension` and `Sweep` go on as numbers: a word is a second spelling to
/// keep in step, and a shell that does not know a kind can still draw the node
/// and say so.
enum class NodeKind : std::uint32_t {
    /// The file, decoded. Where the audio comes from.
    source = 0,
    /// Path B's converter: the f64 bus, the gain, the dither and the shaping.
    /// Present only when the run is processed (§5).
    convert = 1,
    /// One `MP_KIND_DSP` stage, in the order it runs.
    dsp = 2,
    /// The device.
    sink = 3,
    /// The video decoder.
    video_source = 4,
    /// One `MP_KIND_VDSP` stage, in linear light inside the presenter (§9.8.3).
    video_stage = 5,
    /// The presenter, which is where the colour pipeline and the display are.
    presenter = 6,
};

enum : std::uint32_t {
    /// A person may take this node out. False for the ends of the chain: a run
    /// with no source or no sink is not a run.
    MP_NODE_REMOVABLE = 1u << 0,
    /// `node_settings` will answer for it.
    MP_NODE_SETTABLE = 1u << 1,
};

/// One node.
struct Node {
    /// Stable for as long as the shape is: `dsp.0`, `vdsp.1`, `sink`. A shell
    /// asks about a node by this and nothing else.
    std::string id;
    /// `NodeKind` as its number.
    std::uint32_t kind = 0;
    /// The module behind it, or empty for a node that is not one -- the
    /// converter is arithmetic in the core and has no module id.
    std::string module;
    /// For people. What a canvas writes on the box.
    std::string name;
    std::uint32_t flags = 0;
};

/// Which output feeds which input, by node id. **A line, not a graph**: §5's
/// Path B is one f64 bus with a linear chain on it, so every node has at most
/// one of each. A canvas that let a person draw an edge from anywhere to
/// anywhere would be offering something the engine will refuse.
struct Edge {
    std::string from;
    std::string to;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

/// One module that is loaded, for the canvas's palette.
struct ModuleRow {
    /// `MpKind` as its number, so a shell that does not know one can still list
    /// it rather than dropping it.
    std::uint32_t kind = 0;
    std::string id;
    std::string name;
    std::uint32_t priority = 0;
    /// Whether §11's `[engine] allow` admits it. A module that is loaded is
    /// admitted by definition; this says whether the list names it, which is
    /// what a person editing that list needs to see.
    bool allowed = true;
};

// --------------------------------------------------------------------------
// Fields
// --------------------------------------------------------------------------

/// Appends fields to a payload. Little-endian, explicitly, byte by byte: a
/// struct written with `memcpy` carries the padding and the byte order of
/// whatever compiled it, and this has to outlive both.
class Writer {
public:
    void u8(std::uint8_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v);
    void f64(double v);
    /// A length and then the bytes. Not NUL-terminated and not fixed-width: a
    /// path has no length a header could reserve for it.
    void str(std::string_view v);
    void format(const Format& f);

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return out_; }
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    void clear() noexcept { out_.clear(); }

private:
    std::vector<std::uint8_t> out_;
};

/// Reads them back.
///
/// Never throws and never reads past the end. Once a read fails the reader is
/// poisoned and every later read returns a default, so a caller may decode a
/// whole message and ask `ok()` once at the end instead of checking every
/// field -- which is the only way that check actually gets written.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    explicit Reader(const std::vector<std::uint8_t>& v) noexcept
        : data_(v.data()), size_(v.size())
    {
    }

    std::uint8_t u8() noexcept;
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    std::int64_t i64() noexcept;
    double f64() noexcept;
    std::string str();
    Format format() noexcept;

    [[nodiscard]] bool ok() const noexcept { return !bad_; }
    [[nodiscard]] bool done() const noexcept { return !bad_ && at_ == size_; }
    /// Read everything, and nothing left over. What a well-formed message is.
    [[nodiscard]] bool complete() const noexcept { return done(); }

private:
    [[nodiscard]] bool take(std::size_t n) noexcept;

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
    bool bad_ = false;
};

// --------------------------------------------------------------------------
// Messages
// --------------------------------------------------------------------------

/// A header and a payload, ready to write to a pipe.
[[nodiscard]] std::vector<std::uint8_t> frame(Kind kind, std::uint32_t id,
                                              const Writer& payload);
[[nodiscard]] std::vector<std::uint8_t> frame(Kind kind, std::uint32_t id);

/// Parses the first `k_header_bytes` of `data`. False when it is not one of
/// ours: the wrong magic, a version this build does not speak, or a length past
/// what any message is allowed to be.
[[nodiscard]] bool parse_header(const std::uint8_t* data, std::size_t size,
                                Header& out) noexcept;

void write(Writer& w, const Status& s);
[[nodiscard]] bool read(Reader& r, Status& s);

void write(Writer& w, const std::vector<Setting>& settings);
[[nodiscard]] bool read(Reader& r, std::vector<Setting>& settings);

void write(Writer& w, const Calibration& c);
[[nodiscard]] bool read(Reader& r, Calibration& c);

void write(Writer& w, const Graph& g);
[[nodiscard]] bool read(Reader& r, Graph& g);

void write(Writer& w, const std::vector<ModuleRow>& modules);
[[nodiscard]] bool read(Reader& r, std::vector<ModuleRow>& modules);

void write_strings(Writer& w, const std::vector<std::string>& items);
[[nodiscard]] bool read_strings(Reader& r, std::vector<std::string>& items);

} // namespace mp::ipc

#endif // MEDIAPERCH_PROTOCOL_HPP

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

    // --- replies, engine to shell ---
    ok = 128,
    error = 129,
    hello_reply = 130,
    status_reply = 131,
    playlist_reply = 132,
    settings_reply = 133,
    log_reply = 134,

    // --- events, engine to shell, unasked ---
    event_state = 200,
    event_log = 201,
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

void write_strings(Writer& w, const std::vector<std::string>& items);
[[nodiscard]] bool read_strings(Reader& r, std::vector<std::string>& items);

} // namespace mp::ipc

#endif // MEDIAPERCH_PROTOCOL_HPP

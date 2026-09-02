// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/protocol.hpp"

#include <cstring>

namespace mp::ipc {
namespace {

/// The largest string this will build. A path is a few hundred bytes; a
/// megabyte is a length somebody made up.
constexpr std::uint32_t k_max_string = k_max_payload;

/// How many items a count field may claim before it is a claim rather than a
/// count. Every list here is a playlist, a settings tree or a log tail.
constexpr std::uint32_t k_max_items = 1u << 16;

} // namespace

const char* state_name(State s) noexcept
{
    switch (s) {
    case State::stopped:
        return "stopped";
    case State::playing:
        return "playing";
    case State::paused:
        return "paused";
    }
    return "?";
}

const char* kind_name(Kind k) noexcept
{
    switch (k) {
    case Kind::hello:
        return "hello";
    case Kind::status:
        return "status";
    case Kind::play:
        return "play";
    case Kind::enqueue:
        return "enqueue";
    case Kind::clear:
        return "clear";
    case Kind::pause:
        return "pause";
    case Kind::resume:
        return "resume";
    case Kind::stop:
        return "stop";
    case Kind::seek:
        return "seek";
    case Kind::next:
        return "next";
    case Kind::previous:
        return "previous";
    case Kind::playlist:
        return "playlist";
    case Kind::settings:
        return "settings";
    case Kind::setting_set:
        return "setting_set";
    case Kind::log:
        return "log";
    case Kind::subscribe:
        return "subscribe";
    case Kind::quit:
        return "quit";
    case Kind::save:
        return "save";
    case Kind::ok:
        return "ok";
    case Kind::error:
        return "error";
    case Kind::hello_reply:
        return "hello_reply";
    case Kind::status_reply:
        return "status_reply";
    case Kind::playlist_reply:
        return "playlist_reply";
    case Kind::settings_reply:
        return "settings_reply";
    case Kind::log_reply:
        return "log_reply";
    case Kind::event_state:
        return "event_state";
    case Kind::event_log:
        return "event_log";
    }
    return "unknown";
}

// --------------------------------------------------------------------------

void Writer::u8(std::uint8_t v)
{
    out_.push_back(v);
}

void Writer::u32(std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

void Writer::u64(std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

void Writer::i64(std::int64_t v)
{
    u64(static_cast<std::uint64_t>(v));
}

void Writer::f64(double v)
{
    // Through the bits rather than through text: a double that survives a round
    // trip as decimal is a double somebody rounded.
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}

void Writer::str(std::string_view v)
{
    const auto n = static_cast<std::uint32_t>(v.size());
    u32(n);
    out_.insert(out_.end(), v.begin(), v.end());
}

void Writer::format(const Format& f)
{
    u32(f.sample_rate);
    u32(f.channels);
    u32(f.channel_mask);
    u32(static_cast<std::uint32_t>(f.sample_type));
    u32(static_cast<std::uint32_t>(f.encoding));
    u32(f.valid_bits);
}

// --------------------------------------------------------------------------

bool Reader::take(std::size_t n) noexcept
{
    if (bad_ || size_ - at_ < n) {
        bad_ = true;
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() noexcept
{
    if (!take(1)) {
        return 0;
    }
    return data_[at_++];
}

std::uint32_t Reader::u32() noexcept
{
    if (!take(4)) {
        return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(data_[at_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    at_ += 4;
    return v;
}

std::uint64_t Reader::u64() noexcept
{
    if (!take(8)) {
        return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(data_[at_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    at_ += 8;
    return v;
}

std::int64_t Reader::i64() noexcept
{
    return static_cast<std::int64_t>(u64());
}

double Reader::f64() noexcept
{
    const std::uint64_t bits = u64();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string Reader::str()
{
    const std::uint32_t n = u32();
    // Checked before it is trusted with an allocation. A length is the one
    // field in a message that can cost something on its own.
    if (n > k_max_string || !take(n)) {
        bad_ = true;
        return {};
    }
    std::string out(reinterpret_cast<const char*>(data_ + at_), n);
    at_ += n;
    return out;
}

Format Reader::format() noexcept
{
    Format f{};
    f.sample_rate = u32();
    f.channels = u32();
    f.channel_mask = u32();
    f.sample_type = static_cast<SampleType>(u32());
    f.encoding = static_cast<Encoding>(u32());
    f.valid_bits = u32();
    return f;
}

// --------------------------------------------------------------------------

std::vector<std::uint8_t> frame(Kind kind, std::uint32_t id, const Writer& payload)
{
    Writer head;
    head.u32(k_magic);
    // Version and kind are 16 bits each, written in the same order the header
    // declares them.
    head.u8(static_cast<std::uint8_t>(k_version & 0xFFu));
    head.u8(static_cast<std::uint8_t>((k_version >> 8) & 0xFFu));
    const auto k = static_cast<std::uint16_t>(kind);
    head.u8(static_cast<std::uint8_t>(k & 0xFFu));
    head.u8(static_cast<std::uint8_t>((k >> 8) & 0xFFu));
    head.u32(id);
    head.u32(static_cast<std::uint32_t>(payload.size()));

    std::vector<std::uint8_t> out = head.bytes();
    out.insert(out.end(), payload.bytes().begin(), payload.bytes().end());
    return out;
}

std::vector<std::uint8_t> frame(Kind kind, std::uint32_t id)
{
    const Writer empty;
    return frame(kind, id, empty);
}

bool parse_header(const std::uint8_t* data, std::size_t size, Header& out) noexcept
{
    if (data == nullptr || size < k_header_bytes) {
        return false;
    }
    Reader r{data, k_header_bytes};
    out.magic = r.u32();
    // One call per statement: the order of two reads either side of an operator
    // is not something C++ promises, and a header parsed backwards on one
    // compiler is exactly the kind of bug this format exists to avoid.
    const std::uint8_t version_low = r.u8();
    const std::uint8_t version_high = r.u8();
    const std::uint8_t kind_low = r.u8();
    const std::uint8_t kind_high = r.u8();
    out.version = static_cast<std::uint16_t>(version_low | (version_high << 8));
    out.kind = static_cast<std::uint16_t>(kind_low | (kind_high << 8));
    out.id = r.u32();
    out.payload = r.u32();
    if (!r.ok() || out.magic != k_magic) {
        return false;
    }
    // A version mismatch is not a message this build can read *at all*: the
    // fields may have moved. Saying no here is better than a shell and an
    // engine agreeing on a length and disagreeing on everything else.
    if (out.version != k_version || out.payload > k_max_payload) {
        return false;
    }
    return true;
}

void write(Writer& w, const Status& s)
{
    w.u32(static_cast<std::uint32_t>(s.state));
    w.u32(s.index);
    w.u32(s.count);
    w.u64(s.position);
    w.u64(s.length);
    w.str(s.track);
    w.str(s.decoder);
    w.str(s.device);
    w.format(s.source);
    w.format(s.wire);
    w.u32(s.fidelity);
    w.u8(s.processed ? 1u : 0u);
    w.u64(s.frames_rendered);
    w.u64(s.underruns);
    w.str(s.error);
}

bool read(Reader& r, Status& s)
{
    s.state = static_cast<State>(r.u32());
    s.index = r.u32();
    s.count = r.u32();
    s.position = r.u64();
    s.length = r.u64();
    s.track = r.str();
    s.decoder = r.str();
    s.device = r.str();
    s.source = r.format();
    s.wire = r.format();
    s.fidelity = r.u32();
    s.processed = r.u8() != 0;
    s.frames_rendered = r.u64();
    s.underruns = r.u64();
    s.error = r.str();
    return r.ok();
}

void write(Writer& w, const std::vector<Setting>& settings)
{
    w.u32(static_cast<std::uint32_t>(settings.size()));
    for (const Setting& s : settings) {
        w.str(s.key);
        w.str(s.value);
        w.str(s.description);
    }
}

bool read(Reader& r, std::vector<Setting>& settings)
{
    const std::uint32_t n = r.u32();
    if (!r.ok() || n > k_max_items) {
        return false;
    }
    settings.clear();
    settings.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        Setting s;
        s.key = r.str();
        s.value = r.str();
        s.description = r.str();
        if (!r.ok()) {
            return false;
        }
        settings.push_back(std::move(s));
    }
    return r.ok();
}

void write_strings(Writer& w, const std::vector<std::string>& items)
{
    w.u32(static_cast<std::uint32_t>(items.size()));
    for (const std::string& s : items) {
        w.str(s);
    }
}

bool read_strings(Reader& r, std::vector<std::string>& items)
{
    const std::uint32_t n = r.u32();
    if (!r.ok() || n > k_max_items) {
        return false;
    }
    items.clear();
    items.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        items.push_back(r.str());
        if (!r.ok()) {
            return false;
        }
    }
    return r.ok();
}

} // namespace mp::ipc

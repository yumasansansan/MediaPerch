// SPDX-License-Identifier: GPL-3.0-or-later

#include "mp4.hpp"

#include <cstring>
#include <utility>

namespace mp::mp4 {
namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

/// A cursor over a byte range that cannot leave it.
///
/// Every box in an MP4 declares its own length, and every one of those lengths
/// came from the file. So each is checked against the range it sits in, and a
/// child box is given a range that is a strict subrange of its parent -- which
/// is what stops a nested length from reaching back out into the rest of the
/// buffer.
class Span {
public:
    Span(const std::uint8_t* data, std::size_t bytes) noexcept : p_(data), n_(bytes) {}

    [[nodiscard]] std::size_t left() const noexcept { return n_ - at_; }
    [[nodiscard]] bool has(std::size_t n) const noexcept { return left() >= n; }
    [[nodiscard]] const std::uint8_t* here() const noexcept { return p_ + at_; }
    void skip(std::size_t n) noexcept { at_ += n < left() ? n : left(); }

    std::uint8_t u8() noexcept { return has(1) ? p_[at_++] : 0u; }

    std::uint16_t u16() noexcept
    {
        const std::uint32_t hi = u8();
        return static_cast<std::uint16_t>((hi << 8) | u8());
    }

    std::uint32_t u32() noexcept
    {
        const std::uint32_t hi = u16();
        return (hi << 16) | u16();
    }

    std::uint64_t u64() noexcept
    {
        const std::uint64_t hi = u32();
        return (hi << 32) | u32();
    }

    /// Reads a box header and returns its payload as its own Span, advancing
    /// past the whole box. False when there is no complete box left.
    bool next_box(std::uint32_t& type, Span& payload) noexcept
    {
        if (!has(8)) {
            return false;
        }
        const std::size_t start = at_;
        std::uint64_t size = u32();
        type = u32();
        std::size_t header = 8;
        if (size == 1) {
            if (!has(8)) {
                return false;
            }
            size = u64();
            header = 16;
        } else if (size == 0) {
            // "to the end of the enclosing box", which is this span.
            size = static_cast<std::uint64_t>(n_ - start);
        }
        if (size < header || size - header > left()) {
            return false;
        }
        const std::size_t body = static_cast<std::size_t>(size - header);
        payload = Span{p_ + at_, body};
        at_ = start + static_cast<std::size_t>(size);
        return true;
    }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t at_ = 0;
};

struct SampleTable {
    std::vector<std::uint8_t> cookie;
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint64_t> chunk_offsets;
    // first_chunk (1-based), samples_per_chunk
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stsc;
    std::uint64_t total_frames = 0;
    bool have_alac = false;
};

/// The ALAC magic cookie lives in a child `alac` box of the `alac` sample entry.
bool read_alac_entry(Span entry, SampleTable& t) noexcept
{
    // AudioSampleEntry: 6 reserved + 2 data_reference_index, then the QuickTime
    // sound description whose version decides how much more there is.
    entry.skip(8);
    const std::uint16_t version = entry.u16();
    entry.skip(6);  // revision + vendor
    entry.skip(8);  // channels, sample size, compression id, packet size
    entry.skip(4);  // sample rate, 16.16
    if (version == 1) {
        entry.skip(16);
    } else if (version == 2) {
        entry.skip(36);
    }

    std::uint32_t type = 0;
    Span child{nullptr, 0};
    while (entry.next_box(type, child)) {
        if (type != fourcc('a', 'l', 'a', 'c')) {
            continue;
        }
        child.skip(4); // version and flags
        if (!child.has(24)) {
            return false;
        }
        t.cookie.assign(child.here(), child.here() + 24);
        t.have_alac = true;
        return true;
    }
    return false;
}

void read_stbl(Span stbl, SampleTable& t) noexcept
{
    std::uint32_t type = 0;
    Span box{nullptr, 0};
    while (stbl.next_box(type, box)) {
        if (type == fourcc('s', 't', 's', 'd')) {
            box.skip(4);
            const std::uint32_t entries = box.u32();
            std::uint32_t etype = 0;
            Span entry{nullptr, 0};
            for (std::uint32_t i = 0; i < entries && box.next_box(etype, entry); ++i) {
                if (etype == fourcc('a', 'l', 'a', 'c') && read_alac_entry(entry, t)) {
                    break;
                }
            }
        } else if (type == fourcc('s', 't', 's', 'z')) {
            box.skip(4);
            const std::uint32_t uniform = box.u32();
            const std::uint32_t count = box.u32();
            if (count > k_max_packets) {
                return;
            }
            t.sizes.clear();
            if (uniform != 0) {
                t.sizes.assign(count, uniform);
            } else {
                if (!box.has(static_cast<std::size_t>(count) * 4u)) {
                    return;
                }
                t.sizes.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    t.sizes.push_back(box.u32());
                }
            }
        } else if (type == fourcc('s', 't', 'c', 'o') || type == fourcc('c', 'o', '6', '4')) {
            const bool wide = (type == fourcc('c', 'o', '6', '4'));
            box.skip(4);
            const std::uint32_t count = box.u32();
            if (count > k_max_packets) {
                return;
            }
            if (!box.has(static_cast<std::size_t>(count) * (wide ? 8u : 4u))) {
                return;
            }
            t.chunk_offsets.clear();
            t.chunk_offsets.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                t.chunk_offsets.push_back(wide ? box.u64() : box.u32());
            }
        } else if (type == fourcc('s', 't', 's', 'c')) {
            box.skip(4);
            const std::uint32_t count = box.u32();
            if (count > k_max_packets) {
                return;
            }
            if (!box.has(static_cast<std::size_t>(count) * 12u)) {
                return;
            }
            t.stsc.clear();
            t.stsc.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t first = box.u32();
                const std::uint32_t per = box.u32();
                box.u32(); // sample description index
                t.stsc.emplace_back(first, per);
            }
        } else if (type == fourcc('s', 't', 't', 's')) {
            box.skip(4);
            const std::uint32_t count = box.u32();
            if (!box.has(static_cast<std::size_t>(count) * 8u)) {
                return;
            }
            std::uint64_t total = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint64_t n = box.u32();
                const std::uint64_t delta = box.u32();
                total += n * delta;
            }
            t.total_frames = total;
        }
    }
}

/// Walks nested boxes looking for one path, because `trak/mdia/minf/stbl` is the
/// only path this parser cares about.
void find_stbl(Span parent, SampleTable& t, int depth) noexcept
{
    if (depth > 8) {
        return;
    }
    std::uint32_t type = 0;
    Span box{nullptr, 0};
    while (parent.next_box(type, box)) {
        if (type == fourcc('s', 't', 'b', 'l')) {
            read_stbl(box, t);
            if (t.have_alac) {
                return;
            }
        } else if (type == fourcc('t', 'r', 'a', 'k') || type == fourcc('m', 'd', 'i', 'a') ||
                   type == fourcc('m', 'i', 'n', 'f')) {
            find_stbl(box, t, depth + 1);
            if (t.have_alac) {
                return;
            }
        }
    }
}

/// Expands the chunk table into one entry per packet.
bool flatten(const SampleTable& t, AudioTrack& out, const char** why) noexcept
{
    if (t.sizes.empty()) {
        *why = "the track has no sample sizes";
        return false;
    }
    if (t.chunk_offsets.empty() || t.stsc.empty()) {
        *why = "the track has no chunk table";
        return false;
    }

    out.packets.clear();
    out.packets.reserve(t.sizes.size());

    std::size_t sample = 0;
    const std::size_t chunks = t.chunk_offsets.size();

    for (std::size_t run = 0; run < t.stsc.size() && sample < t.sizes.size(); ++run) {
        const std::uint32_t first = t.stsc[run].first;
        const std::uint32_t per = t.stsc[run].second;
        if (first == 0 || per == 0) {
            *why = "the chunk table describes an empty or zero-based run";
            return false;
        }
        const std::size_t begin = first - 1u;
        const std::size_t end =
            (run + 1 < t.stsc.size()) ? (t.stsc[run + 1].first - 1u) : chunks;
        if (begin >= chunks || end > chunks || end < begin) {
            *why = "the chunk table points outside the chunk offsets";
            return false;
        }

        for (std::size_t c = begin; c < end && sample < t.sizes.size(); ++c) {
            std::uint64_t at = t.chunk_offsets[c];
            for (std::uint32_t i = 0; i < per && sample < t.sizes.size(); ++i) {
                out.packets.push_back(Packet{at, t.sizes[sample]});
                at += t.sizes[sample];
                ++sample;
            }
        }
    }

    if (out.packets.empty()) {
        *why = "the chunk table yielded no packets";
        return false;
    }
    return true;
}

} // namespace

bool parse_moov(const std::uint8_t* data, std::size_t bytes, AudioTrack& out,
                const char** why) noexcept
{
    static const char* unused = "";
    if (why == nullptr) {
        why = &unused;
    }
    *why = "";
    if (data == nullptr || bytes == 0) {
        *why = "empty moov";
        return false;
    }

    SampleTable t;
    find_stbl(Span{data, bytes}, t, 0);

    if (!t.have_alac) {
        *why = "no ALAC track in this file";
        return false;
    }

    out.cookie = t.cookie;
    out.total_frames = t.total_frames;
    return flatten(t, out, why);
}

} // namespace mp::mp4

// SPDX-License-Identifier: GPL-3.0-or-later

#include "flacframe.hpp"

namespace flacframe {
namespace {

std::uint8_t crc8_byte(std::uint8_t crc, std::uint8_t byte) noexcept
{
    // x^8 + x^2 + x + 1. Eight steps rather than a table: this runs once per
    // frame header and a 256-byte table is not worth the cache line.
    unsigned value = static_cast<unsigned>(crc ^ byte);
    for (int i = 0; i < 8; ++i) {
        value = (value & 0x80u) != 0 ? ((value << 1) ^ 0x07u) : (value << 1);
    }
    return static_cast<std::uint8_t>(value & 0xFFu);
}

std::uint8_t crc8(const std::uint8_t* p, std::size_t bytes) noexcept
{
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        crc = crc8_byte(crc, p[i]);
    }
    return crc;
}

std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

/// The block size a header's four-bit code means, or 0 when it is one of the two
/// codes that put the number in the header instead.
std::uint32_t block_size_of(unsigned code) noexcept
{
    switch (code) {
    case 0: return 0;               // reserved
    case 1: return 192;
    case 2: case 3: case 4: case 5: return 576u << (code - 2);
    case 6: case 7: return 0;       // stated later, in 8 or 16 bits
    default: return 256u << (code - 8);
    }
}

/// The eleven rates a frame header can name in four bits, plus code 0, which
/// means "the one STREAMINFO gave".
///
/// **This is not the set of rates FLAC supports** -- that is 1 Hz to 1048575 Hz,
/// carried in STREAMINFO's twenty-bit field, and `parse_streaminfo` reads all of
/// it. The four-bit code is a shorthand for the common ones; codes 12, 13 and 14
/// spell an arbitrary rate out in the header instead, and the caller handles
/// those. Zero from here means "not one of the eleven", which the caller then
/// either fills in or leaves for STREAMINFO to answer.
std::uint32_t rate_of(unsigned code) noexcept
{
    static const std::uint32_t rates[12] = {0,     88200, 176400, 192000, 8000,  16000,
                                            22050, 24000, 32000,  44100,  48000, 96000};
    return code < 12 ? rates[code] : 0;
}

std::uint32_t bits_of(unsigned code) noexcept
{
    // Code 3 was reserved until FLAC 1.4 gave code 7 to 32 bits; dr_flac still
    // has the old table, which is why it decodes a 32-bit FLAC to nothing.
    static const std::uint32_t bits[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    return code < 8 ? bits[code] : 0;
}

} // namespace

std::uint16_t crc16_byte(std::uint16_t crc, std::uint8_t byte) noexcept
{
    // x^16 + x^15 + x^2 + 1.
    unsigned value = static_cast<unsigned>(crc) ^ (static_cast<unsigned>(byte) << 8);
    for (int i = 0; i < 8; ++i) {
        value = (value & 0x8000u) != 0 ? ((value << 1) ^ 0x8005u) : (value << 1);
    }
    return static_cast<std::uint16_t>(value & 0xFFFFu);
}

std::uint16_t crc16(std::uint16_t crc, const std::uint8_t* p, std::size_t bytes) noexcept
{
    for (std::size_t i = 0; i < bytes; ++i) {
        crc = crc16_byte(crc, p[i]);
    }
    return crc;
}

bool parse_streaminfo(const std::uint8_t* p, std::size_t bytes, StreamInfo& out) noexcept
{
    if (p == nullptr || bytes < 34) {
        return false;
    }
    out = StreamInfo{};
    out.min_block = (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
    out.max_block = (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    out.min_frame = (static_cast<std::uint32_t>(p[4]) << 16) |
                    (static_cast<std::uint32_t>(p[5]) << 8) | p[6];
    out.max_frame = (static_cast<std::uint32_t>(p[7]) << 16) |
                    (static_cast<std::uint32_t>(p[8]) << 8) | p[9];

    const std::uint32_t packed = be32(p + 10);
    out.sample_rate = packed >> 12;
    out.channels = ((packed >> 9) & 0x7u) + 1;
    out.bits = ((packed >> 4) & 0x1Fu) + 1;
    out.total_samples = (static_cast<std::uint64_t>(packed & 0xFu) << 32) | be32(p + 14);

    // **The rate is twenty bits and every value in it is legal**: FLAC allows
    // 1 Hz to 1048575 Hz, so there is no ceiling to check here and imposing one
    // would refuse files the format permits. Zero is the single exception -- it
    // is what a FLAC stream inside a container that states the rate elsewhere is
    // allowed to write, and a decoder handed one would have nothing to report.
    //
    // The other three bounds are the fields' own: eight channels, thirty-two
    // bits, and a block size that fits in the sixteen bits the frame header has
    // for it.
    return out.sample_rate != 0 && out.sample_rate <= 1048575 && out.channels >= 1 &&
           out.channels <= 8 && out.bits >= 4 && out.bits <= 32 && out.min_block >= 16 &&
           out.max_block >= out.min_block && out.max_block <= 65535;
}

bool parse_header(const std::uint8_t* p, std::size_t bytes, FrameHeader& out) noexcept
{
    if (p == nullptr || bytes < 5) {
        return false;
    }
    // Fourteen bits of sync, then a reserved bit that must be zero, then the
    // blocking strategy.
    if (p[0] != 0xFFu || (p[1] & 0xFCu) != 0xF8u) {
        return false;
    }
    out = FrameHeader{};
    out.variable = (p[1] & 0x01u) != 0;

    const unsigned block_code = (p[2] >> 4) & 0xFu;
    const unsigned rate_code = p[2] & 0xFu;
    const unsigned channel_code = (p[3] >> 4) & 0xFu;
    const unsigned bits_code = (p[3] >> 1) & 0x7u;
    if (block_code == 0 || rate_code == 15 || channel_code > 10 || bits_code == 3 ||
        (p[3] & 0x01u) != 0) {
        return false;
    }
    // Eight codes are a channel count; three are the stereo decorrelations, all
    // of which are two channels once decoded.
    out.channels = channel_code < 8 ? channel_code + 1 : 2;
    out.bits = bits_of(bits_code);

    std::size_t at = 4;

    // The coded number is UTF-8's encoding used for an integer: 31 bits for a
    // frame number, 36 for a sample number, which is the one case that needs a
    // seventh byte and the one place this differs from UTF-8 proper.
    const std::uint8_t first = p[at];
    unsigned extra = 0;
    std::uint64_t value = 0;
    if ((first & 0x80u) == 0) {
        value = first;
    } else if ((first & 0xE0u) == 0xC0u) {
        extra = 1;
        value = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        extra = 2;
        value = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        extra = 3;
        value = first & 0x07u;
    } else if ((first & 0xFCu) == 0xF8u) {
        extra = 4;
        value = first & 0x03u;
    } else if ((first & 0xFEu) == 0xFCu) {
        extra = 5;
        value = first & 0x01u;
    } else if (first == 0xFEu) {
        extra = 6;
        value = 0;
    } else {
        return false; // a continuation byte where a leading one belongs
    }
    ++at;
    if (at + extra > bytes) {
        return false;
    }
    for (unsigned i = 0; i < extra; ++i) {
        if ((p[at] & 0xC0u) != 0x80u) {
            return false;
        }
        value = (value << 6) | (p[at] & 0x3Fu);
        ++at;
    }
    out.number = value;

    out.block_size = block_size_of(block_code);
    if (block_code == 6) {
        if (at + 1 > bytes) {
            return false;
        }
        out.block_size = static_cast<std::uint32_t>(p[at]) + 1;
        at += 1;
    } else if (block_code == 7) {
        if (at + 2 > bytes) {
            return false;
        }
        out.block_size =
            ((static_cast<std::uint32_t>(p[at]) << 8) | p[at + 1]) + 1;
        at += 2;
    }

    // Codes 12 to 14 are how a frame names a rate that is not one of the eleven:
    // whole kHz in eight bits, Hz in sixteen, or tens of Hz in sixteen. Between
    // them they reach 655350 Hz; a stream faster than that -- FLAC allows up to
    // 1048575 Hz -- writes code 0 and lets STREAMINFO answer, which is why a
    // zero here is not a failure.
    out.sample_rate = rate_of(rate_code);
    if (rate_code == 12) {
        if (at + 1 > bytes) {
            return false;
        }
        out.sample_rate = static_cast<std::uint32_t>(p[at]) * 1000u;
        at += 1;
    } else if (rate_code == 13 || rate_code == 14) {
        if (at + 2 > bytes) {
            return false;
        }
        const std::uint32_t stated = (static_cast<std::uint32_t>(p[at]) << 8) | p[at + 1];
        out.sample_rate = rate_code == 13 ? stated : stated * 10u;
        at += 2;
    }

    if (at + 1 > bytes) {
        return false;
    }
    if (crc8(p, at) != p[at]) {
        return false;
    }
    out.header_bytes = static_cast<std::uint32_t>(at + 1);
    return out.block_size != 0;
}

bool matches(const FrameHeader& h, const StreamInfo& info) noexcept
{
    if (h.channels != info.channels) {
        return false;
    }
    if (h.sample_rate != 0 && h.sample_rate != info.sample_rate) {
        return false;
    }
    if (h.bits != 0 && h.bits != info.bits) {
        return false;
    }
    return h.block_size <= info.max_block;
}

std::size_t frame_length(const std::uint8_t* p, std::size_t bytes, const StreamInfo& info,
                         bool at_end) noexcept
{
    FrameHeader here{};
    if (!parse_header(p, bytes, here) || !matches(here, info)) {
        return 0;
    }

    // The smallest a frame could be: its header, one byte, and the CRC. The
    // encoder's own minimum is better when it wrote one.
    std::size_t least = here.header_bytes + 3;
    if (info.min_frame > least) {
        least = info.min_frame;
    }
    // And the largest, so a corrupt file cannot make this scan the whole of it.
    std::size_t most = bytes;
    if (info.max_frame != 0 && info.max_frame < most) {
        most = info.max_frame;
    }
    // **A header can parse in fewer bytes than the shortest frame needs**, and
    // then the running CRC below would start by reading past the end of what it
    // was given. Found by the fuzzer on eighteen bytes that begin with an MPEG
    // sync -- 0xFF 0xFB also satisfies FLAC's fourteen-bit one -- and reachable
    // for real on a file whose last frame is truncated.
    if (least > bytes || least > most) {
        return 0;
    }

    // **The CRC is what says where the frame ends.** It is run forward one byte
    // at a time, so every candidate end costs nothing beyond the byte itself,
    // and a position where it reads zero is the frame agreeing with its own
    // checksum. A frame header at that same position -- or the end of the audio,
    // for the last frame, which has no successor to find -- turns that from
    // likely into certain.
    std::uint16_t crc = crc16(0, p, least);
    for (std::size_t end = least; end <= most; ++end) {
        if (crc == 0) {
            if (end == bytes) {
                if (at_end) {
                    return end;
                }
            } else {
                FrameHeader next{};
                if (parse_header(p + end, bytes - end, next) && matches(next, info)) {
                    return end;
                }
            }
        }
        if (end == most) {
            break;
        }
        crc = crc16_byte(crc, p[end]);
    }
    return 0;
}

} // namespace flacframe

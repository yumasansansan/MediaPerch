// SPDX-License-Identifier: GPL-3.0-or-later
//
// Just enough MP4 to find one ALAC track and its packet boundaries.
//
// This is not a general demuxer and is not trying to be. It reads `moov` and
// answers three questions: where is the magic cookie, where is each packet, and
// how many frames are there. Everything else in the file is somebody else's
// problem -- which is the only reason a parser for a container this baroque can
// be a few hundred lines and still be defensible.
//
// `moov` is handed in as a buffer because it is small and self-contained; the
// packets are left where they are, so a two-hour album is not a two-hour
// allocation. That split is also what makes this file fuzzable on its own.

#ifndef MEDIAPERCH_MP4_HPP
#define MEDIAPERCH_MP4_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp::mp4 {

struct Packet {
    std::uint64_t offset; ///< from the start of the file
    std::uint32_t size;
};

struct AudioTrack {
    std::vector<std::uint8_t> cookie; ///< the ALACSpecificConfig, 24 bytes or more
    std::vector<Packet> packets;
    std::uint64_t total_frames = 0;
    std::uint32_t timescale = 0;
};

/// Parses the *payload* of a `moov` box -- everything after its 8-byte header.
/// `why` is set to a fixed string on failure and is never allocated.
bool parse_moov(const std::uint8_t* data, std::size_t bytes, AudioTrack& out,
                const char** why) noexcept;

/// A hard bound on how many packets a file may claim, so that a corrupt sample
/// table cannot ask for an arbitrary allocation before anything has been read.
/// At ALAC's 4096 frames per packet this is about eleven hours at 48 kHz.
inline constexpr std::size_t k_max_packets = 4u * 1000u * 1000u;

} // namespace mp::mp4

#endif // MEDIAPERCH_MP4_HPP

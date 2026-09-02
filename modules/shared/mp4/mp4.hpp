// SPDX-License-Identifier: GPL-3.0-or-later
//
// Just enough MP4 to find one audio track, its packet boundaries, its codec
// configuration and its gapless edit.
//
// This is not a general demuxer and is not trying to be. It reads `moov` and
// answers four questions: what codec is this, where is its configuration, where
// is each packet, and how much of the decoded audio does the file actually
// claim. Everything else is somebody else's problem -- which is the only reason
// a parser for a container this baroque can be a few hundred lines and still be
// defensible.
//
// It started inside decode_alac and moved out when decode_aac needed the same
// four answers. Both of those are gone; `demux_mp4` is what asks now, and the
// answers did not change, which is the clearest evidence available that the
// v2 split fell where the seam already was. The parts that differ between the two codecs are exactly two
// boxes: `alac` carries its cookie in a child box of the same name, `mp4a`
// carries an AudioSpecificConfig inside an `esds` descriptor chain.
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

/// Four-character codes, as they appear in `stsd`.
inline constexpr std::uint32_t k_codec_alac = 0x616C6163u; ///< 'alac'
inline constexpr std::uint32_t k_codec_mp4a = 0x6D703461u; ///< 'mp4a'

struct AudioTrack {
    std::uint32_t codec = 0;          ///< k_codec_alac, k_codec_mp4a, or 0
    std::vector<std::uint8_t> config; ///< ALACSpecificConfig or AudioSpecificConfig
    std::vector<Packet> packets;

    /// Every frame the packets decode to, including the encoder's warm-up.
    std::uint64_t total_frames = 0;

    /// The gapless edit, from `elst`, in media frames.
    ///
    /// `skip_frames` is the encoder delay the file says to discard before the
    /// audio begins; `play_frames` is how much to emit after that, or 0 when the
    /// file does not say. This is the whole of what separates a decoder that
    /// starts a track 21 milliseconds late from one that does not, and it is two
    /// numbers in a box most demuxers skip.
    std::uint64_t skip_frames = 0;
    std::uint64_t play_frames = 0;

    /// The duration of a whole packet, from the first `stts` entry.
    ///
    /// Not `total_frames / packets`: the last packet of a track is usually
    /// short, so the average is a few frames under the real thing -- 1021
    /// instead of 1024 on a two-second file, which is exactly wrong enough to
    /// be hard to see.
    std::uint32_t frames_per_packet = 0;

    std::uint32_t media_timescale = 0; ///< from `mdhd`; the sample rate, for audio
    std::uint32_t movie_timescale = 0; ///< from `mvhd`; what `elst` durations are in
};

/// Parses the *payload* of a `moov` box -- everything after its 8-byte header.
/// `why` is set to a fixed string on failure and is never allocated.
bool parse_moov(const std::uint8_t* data, std::size_t bytes, AudioTrack& out,
                const char** why) noexcept;

/// A hard bound on how many packets a file may claim, so that a corrupt sample
/// table cannot ask for an arbitrary allocation before anything has been read.
/// At AAC's 1024 frames per packet this is about a day at 48 kHz.
inline constexpr std::size_t k_max_packets = 4u * 1000u * 1000u;

} // namespace mp::mp4

#endif // MEDIAPERCH_MP4_HPP

// SPDX-License-Identifier: GPL-3.0-or-later
//
// AVCC to Annex B, which is a container question wearing a codec's clothes.
//
// **MP4 does not store H.264 the way a decoder wants it.** `avcC` holds the
// sequence and picture parameter sets out of band, and each sample is a list of
// NAL units prefixed by their length -- one, two or four bytes, stated in the
// `avcC`. Every H.264 decoder on Windows wants Annex B instead: the parameter
// sets in the stream, and each NAL preceded by a `00 00 00 01` start code.
//
// So something has to convert, and it is this module rather than the demuxer,
// because `demux_mp4` hands over the container's bytes verbatim and that is the
// promise that makes it checkable. The conversion is bounded, it is a codec's
// idea of its own bitstream, and it is separated into a header of its own so
// that it can be tested without Media Foundation, a GPU or a file.
//
// HEVC is the same shape with a different configuration record (`hvcC`), which
// stores its parameter sets in arrays rather than in two lists. The parsing is
// separate; the emitting is identical, which is why `Annex` takes them as a
// sequence rather than as SPS and PPS.

#ifndef MEDIAPERCH_CODEC_MFT_AVCC_HPP
#define MEDIAPERCH_CODEC_MFT_AVCC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp::mft {

/// What an `avcC` says, minus the parts a decoder is told by the stream.
struct AvcConfig {
    /// One, two or four. **The field is `lengthSizeMinusOne` and this is the
    /// value plus one**, which is the sort of thing that reads correctly and is
    /// wrong by one for a year.
    std::uint32_t length_size = 4;
    /// The parameter sets, in the order they must be emitted, each without a
    /// start code. SPS first, then PPS, which is the order `avcC` states them
    /// and the order a decoder needs them.
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    bool valid = false;
};

/// Parses an `avcC` box body.
///
/// Returns a config with `valid == false` for anything it cannot read rather
/// than throwing or guessing: a malformed record is a file this module declines,
/// and declining is what lets `demux_mp4` go on trusting its own parser.
[[nodiscard]] AvcConfig parse_avcc(const std::uint8_t* data, std::size_t bytes);

/// Rewrites one AVCC sample as Annex B, appending to `out`.
///
/// `with_parameter_sets` prepends the SPS and PPS, which a decoder needs before
/// the first frame and again after a seek -- **and which is why they are
/// prepended per keyframe rather than once.** A decoder reset at a seek has
/// forgotten them, and a stream that changes resolution mid-file states new
/// ones in-band anyway.
///
/// False when the sample is malformed: a length that runs past the end is a
/// truncated file, and emitting what is left would hand a decoder a NAL unit
/// that stops in the middle of a slice.
[[nodiscard]] bool to_annex_b(const AvcConfig& config, const std::uint8_t* sample,
                              std::size_t bytes, bool with_parameter_sets,
                              std::vector<std::uint8_t>& out);

} // namespace mp::mft

#endif // MEDIAPERCH_CODEC_MFT_AVCC_HPP

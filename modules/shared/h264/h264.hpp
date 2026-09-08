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

#ifndef MEDIAPERCH_SHARED_H264_HPP
#define MEDIAPERCH_SHARED_H264_HPP

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

/// What a sequence parameter set says about the shape of its pixels.
///
/// **Four fields, and the reason to read them is refusal.** Every H.264 decoder
/// this tree can reach -- Media Foundation's, the fixed-function block behind
/// it, openh264 if it is ever added -- decodes 8-bit 4:2:0 and nothing else.
/// The high profiles reach 4:2:2, 4:4:4 and up to fourteen bits, and a decoder
/// handed one of those either fails somewhere unhelpful or, worse, produces a
/// picture that is quietly wrong. The container says which before anything is
/// opened, so a `probe` can decline and the host can look elsewhere -- which is
/// §7's rule that a decoder failing mid-file must never trigger a silent retry.
///
/// Deliberately stops after the bit depths. The geometry is `demux_mp4`'s and
/// the colour code points are the container's (§9.8's join), so reading further
/// -- `pic_order_cnt_type`'s branches, the frame cropping, the VUI -- would be
/// syntax with no reader.
struct SpsInfo {
    std::uint32_t profile_idc = 0;
    /// 0 is monochrome, 1 is 4:2:0, 2 is 4:2:2, 3 is 4:4:4. **Inferred as 1
    /// when the profile does not carry it**, which the standard states and
    /// which is why every Baseline and Main stream reads as 4:2:0 here.
    std::uint32_t chroma_format_idc = 1;
    std::uint32_t bit_depth_luma = 8;
    std::uint32_t bit_depth_chroma = 8;
    /// 4:4:4 with the three planes coded as separate monochrome pictures. A
    /// different decode again, and refused wherever 4:4:4 is.
    bool separate_colour_plane = false;
    bool valid = false;
};

/// Reads one SPS NAL unit -- **with its header byte, without a start code**,
/// which is exactly how `avcC` stores it.
///
/// Returns `valid == false` for a NAL that is not an SPS, for one that runs out
/// of bits, and for one whose Exp-Golomb codes are longer than a `uint32_t` can
/// hold. A malformed parameter set is a file to decline rather than to guess
/// at, which is the same answer `parse_avcc` gives.
[[nodiscard]] SpsInfo parse_sps(const std::uint8_t* nal, std::size_t bytes);

/// The first SPS in a parsed `avcC`, read.
[[nodiscard]] SpsInfo sps_of(const AvcConfig& config);

/// What an `hvcC` says, which is an `avcC`'s two useful facts plus the three
/// that let a decoder be declined before it is opened.
///
/// **The same `AvcConfig` comes out**, because the two records differ in how
/// they are written and not in what a decoder needs from them: a length size,
/// and the parameter sets in the order to emit them. `to_annex_b` takes that
/// pair and has never known which record it came from -- which is what the note
/// at the top of this file meant by the emitting being identical.
///
/// The three extra facts are `hvcC`'s own, sitting in bytes 16 to 18 where
/// `avcC` has nothing: the chroma format and the two bit depths. H.264 hides
/// those inside the SPS and this tree parses one to find them; HEVC states them
/// in the record, so a `probe` can decline 4:2:2 or ten bits without reading a
/// bitstream at all.
struct HevcConfig {
    /// Everything `to_annex_b` needs, filled the same way.
    AvcConfig annex;
    /// 0 monochrome, 1 is 4:2:0, 2 is 4:2:2, 3 is 4:4:4. Stated rather than
    /// inferred, unlike H.264's.
    std::uint32_t chroma_format_idc = 1;
    std::uint32_t bit_depth_luma = 8;
    std::uint32_t bit_depth_chroma = 8;
    /// The general_profile_idc: 1 is Main, 2 Main 10, 3 Main Still Picture.
    std::uint32_t profile_idc = 0;
    bool valid = false;
};

/// **How to decode the colour, as an HEVC SPS states it.**
///
/// The three ISO/IEC 23091-2 code points and the range flag, out of the SPS's
/// VUI. `hvcC` does not carry them and MP4's `colr` box is optional -- measured
/// on ffmpeg 9.0.1, its muxer writes no `colr` even for a stream tagged BT.2020
/// and PQ. A container that says nothing leaves `assumed_transfer` at BT.709,
/// which decodes a PQ stream with an SDR curve: §9.1's fault, on a real file.
///
/// `valid` false when the SPS has no VUI, no colour description, or could not
/// be walked. **Not a guess in that case**: unspecified is what the ABI already
/// means by 2, and a wrong code point is worse than an absent one.
struct HevcColour {
    std::uint32_t primaries = 2;
    std::uint32_t transfer = 2;
    std::uint32_t matrix = 2;
    bool full_range = false;
    bool valid = false;
};

/// Reads the first SPS among a config's NALs and returns what its VUI said.
///
/// **This is the one thing here that needs a bitstream**, which is why
/// `parse_hvcc` never did: the record states the chroma format and both bit
/// depths, so a `probe` could decline a stream without reading one. The colour
/// is not in the record.
[[nodiscard]] HevcColour hevc_colour(const AvcConfig& config);

/// The coded size out of the first SPS among a config's NALs: what a decoder
/// is entitled to be told before it offers an output type, and what the
/// record does not state. `valid` false when there is no SPS or it could not
/// be walked that far.
struct HevcSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool valid = false;
};
[[nodiscard]] HevcSize hevc_size(const AvcConfig& config);

/// **What the content was graded on, as HEVC states it.**
///
/// SMPTE ST 2086's mastering display and CTA-861.3's light levels, which HEVC
/// carries as prefix SEI messages -- payload types 137 and 144 -- rather than
/// in the parameter sets. ISO/IEC 14496-12 also defines `mdcv` and `clli` boxes
/// for the same numbers, and **files in the wild carry the SEI**: x265 writes
/// it in band and ffmpeg's MP4 muxer, measured on version 9.0.1, writes neither
/// box. A demuxer that read only the boxes would report nothing for most real
/// HDR10 files.
///
/// Reachable without decoding, because `hvcC` may carry SEI in its arrays and
/// an encoder asked for repeated headers puts it there. `parse_hvcc` already
/// keeps every NAL it finds, in order, and declines to object to an SEI among
/// them; this is what reads one.
///
/// **The order is corrected here.** The SEI states its primaries starting at
/// green, and both `MpVideoInfo` and DXGI want red first. One reorder, in the
/// one place that knows the SEI's convention.
struct HdrMetadata {
    /// Red, green, blue. In units of 0.00002, which is the SEI's own and the
    /// ABI's.
    std::uint32_t primaries_x[3] = {0, 0, 0};
    std::uint32_t primaries_y[3] = {0, 0, 0};
    std::uint32_t white_x = 0;
    std::uint32_t white_y = 0;
    /// In units of 0.0001 cd/m^2, again the SEI's own.
    std::uint32_t max_luminance = 0;
    std::uint32_t min_luminance = 0;
    /// Whole cd/m^2.
    std::uint32_t max_content_light_level = 0;
    std::uint32_t max_frame_average_light_level = 0;
    bool has_mastering = false;
    bool has_light_levels = false;
};

/// Reads both SEI messages out of whatever NALs a config is carrying.
///
/// Everything absent when there are none, which is the common case: static
/// metadata is optional and most streams state none.
[[nodiscard]] HdrMetadata hevc_hdr_metadata(const AvcConfig& config);

/// The same two messages out of one sample -- a list of length-prefixed NAL
/// units, as MP4 and Matroska store them -- for the file that carries them in
/// band only. **Which is where most HDR10 files keep them**: the record holds
/// SEI only when an encoder was asked to repeat its headers, the boxes only
/// when a muxer wrote them, and the first sample is a keyframe with the prefix
/// SEI in front of its slice. Only prefix SEI NALs are copied out, so a 4K
/// keyframe costs a few bytes to scan.
[[nodiscard]] HdrMetadata hevc_hdr_metadata_in_sample(const AvcConfig& config,
                                                       const std::uint8_t* sample,
                                                       std::size_t bytes);

/// Parses an `hvcC` box body.
///
/// Returns `valid == false` for anything it cannot read, which is the answer
/// `parse_avcc` gives and for the same reason: a malformed record is a file to
/// decline rather than to guess at.
[[nodiscard]] HevcConfig parse_hvcc(const std::uint8_t* data, std::size_t bytes);

/// Just the parameter sets, as Annex B, replacing `out`.
///
/// **A decoder wants them before the first sample and again after a reset**,
/// and at neither of those moments is there a sample to attach them to.
/// `to_annex_b` refuses a null one on purpose -- a sample that is not there is
/// a caller's mistake, not an empty sample -- so asking for the sets alone is
/// its own question with its own answer.
///
/// False when the config is not one, which is the only way this can fail.
[[nodiscard]] bool parameter_sets_annex_b(const AvcConfig& config,
                                          std::vector<std::uint8_t>& out);

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

#endif // MEDIAPERCH_SHARED_H264_HPP

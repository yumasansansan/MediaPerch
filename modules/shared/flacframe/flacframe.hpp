// SPDX-License-Identifier: GPL-3.0-or-later
//
// FLAC's framing: STREAMINFO, the frame header, and the two CRCs that make
// finding a frame's end reliable rather than hopeful.
//
// **Why this exists at all.** A FLAC frame does not carry its own length. Every
// FLAC parser therefore finds the end of frame N by finding the beginning of
// frame N+1, and a sync pattern turns up by chance in compressed audio -- so the
// question is what evidence is enough. The answer the format gives is that each
// frame ends with a CRC-16 over itself, which is exactly the confirmation that a
// scan needs, and it can be computed incrementally: run the CRC forward a byte
// at a time and every position where it reads zero is a candidate end. Requiring
// a valid frame header at that position as well brings the chance of a wrong
// split to about one in sixteen million per byte.
//
// So the splitter here is linear in the file and does not guess. That matters
// because `demux_flac` is the only thing between a stranger's file and libFLAC.
//
// Shared rather than private to the demuxer for the reason `shared/mp4` is: the
// fuzz target links it directly, and a parser that can only be reached through a
// module is a parser that cannot be fuzzed on its own.

#ifndef MEDIAPERCH_FLACFRAME_HPP
#define MEDIAPERCH_FLACFRAME_HPP

#include <cstddef>
#include <cstdint>

namespace flacframe {

/// The STREAMINFO block, without its four-byte metadata-block header -- which
/// is exactly the shape the ABI defines for MP_CODEC_FLAC's configuration blob.
struct StreamInfo {
    std::uint32_t min_block = 0;
    std::uint32_t max_block = 0;
    std::uint32_t min_frame = 0; ///< bytes; 0 means the encoder did not say
    std::uint32_t max_frame = 0; ///< bytes; 0 means the encoder did not say
    /// 1 Hz to 1048575 Hz. FLAC's field is twenty bits wide and this reads all
    /// of it: the eleven rates the frame header can name in four bits are a
    /// shorthand, not the format's range.
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    std::uint64_t total_samples = 0; ///< 0 means unknown, per the format
};

/// True when the 34 bytes describe a stream this tree could play.
bool parse_streaminfo(const std::uint8_t* p, std::size_t bytes, StreamInfo& out) noexcept;

/// What a frame header states. Where a field says "from STREAMINFO" the value is
/// left at zero, because that is what the frame said and resolving it is the
/// caller's business.
struct FrameHeader {
    /// The sample number for a variable-blocksize stream, the frame number for a
    /// fixed-blocksize one. `variable` says which.
    std::uint64_t number = 0;
    bool variable = false;
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0; ///< 0: from STREAMINFO
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;        ///< 0: from STREAMINFO
    std::uint32_t header_bytes = 0;
};

/// Parses a frame header, CRC-8 included. False for anything that is not one.
bool parse_header(const std::uint8_t* p, std::size_t bytes, FrameHeader& out) noexcept;

/// Whether a header could belong to the same stream as `info`. A chance sync
/// pattern agrees about almost nothing else.
bool matches(const FrameHeader& h, const StreamInfo& info) noexcept;

/// CRC-16 of a run of bytes, continued from `crc`. Zero over a whole frame --
/// its own trailing CRC included -- is the frame confirming itself.
std::uint16_t crc16(std::uint16_t crc, const std::uint8_t* p, std::size_t bytes) noexcept;
std::uint16_t crc16_byte(std::uint16_t crc, std::uint8_t byte) noexcept;

/// The end of the frame beginning at `p`, as an offset into `p`, or 0 when the
/// bytes given do not contain a whole one.
///
/// `at_end` says that `bytes` is the end of the audio rather than the end of
/// what has been read so far, which is what lets the last frame in a file -- the
/// one with no following header to find -- be recognised.
std::size_t frame_length(const std::uint8_t* p, std::size_t bytes, const StreamInfo& info,
                         bool at_end) noexcept;

} // namespace flacframe

#endif // MEDIAPERCH_FLACFRAME_HPP

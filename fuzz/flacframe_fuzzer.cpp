// SPDX-License-Identifier: GPL-3.0-or-later
//
// FLAC's framing, given arbitrary bytes.
//
// **This is the parser that stands between a stranger's file and libFLAC**, and
// it is ours rather than Xiph's. `demux_flac` has to decide where each frame
// ends before it can hand one over, and it decides that by scanning: running a
// CRC forward and testing every position where it reads zero. A scan over
// attacker-controlled bytes is exactly the shape of code that needs this file.
//
// Three entry points, one input, because they are the three ways bytes reach
// the parser: a STREAMINFO block that the container claims describes the
// stream, a frame header at an arbitrary offset, and the length scan that reads
// past both of them.

#include "flacframe.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    // The configuration blob, which a container hands over unread.
    flacframe::StreamInfo info;
    const bool described = flacframe::parse_streaminfo(data, size, info);

    // A frame header, at the front of whatever this is.
    flacframe::FrameHeader header;
    (void)flacframe::parse_header(data, size, header);

    // And the scan. It is given a plausible stream description whether or not
    // the input contained one, because the interesting case is a demuxer that
    // read a valid STREAMINFO and then met a frame region that disagrees with
    // it -- a truncated file, a corrupt one, or one built to disagree.
    if (!described) {
        info = flacframe::StreamInfo{};
        info.min_block = 4096;
        info.max_block = 4096;
        info.sample_rate = 44100;
        info.channels = 2;
        info.bits = 16;
    }
    (void)flacframe::frame_length(data, size, info, true);
    (void)flacframe::frame_length(data, size, info, false);
    return 0;
}

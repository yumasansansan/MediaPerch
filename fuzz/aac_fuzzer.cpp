// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AAC-LC parser, given arbitrary bytes.
//
// This one earned its place before the decoder was finished. The first version
// of read_section_data had no end-of-packet check in the loop that advances
// through scalefactor bands, so a zero-length section spun forever -- found by
// running the parser over a real file, but exactly what a fuzzer exists to find
// and the reason this target was written alongside the code rather than after.

#include "aac.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 3) {
        return 0;
    }

    // The first two bytes are an AudioSpecificConfig, the rest is a frame.
    mp::aac::Config cfg;
    if (!mp::aac::parse_asc(data, 2, cfg)) {
        return 0;
    }

    static mp::aac::Decoder decoder;
    if (!decoder.init(cfg)) {
        return 0;
    }
    decoder.decode_frame(data + 2, size - 2);

    // Again from a different offset: a second frame exercises the paths that
    // assume a frame already went through.
    if (size > 8) {
        decoder.decode_frame(data + 4, size - 4);
    }
    return 0;
}

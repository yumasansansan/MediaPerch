// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ALAC codec, given arbitrary bytes.
//
// This is the fuzzer that had to exist before an ALAC decoder written here could
// be defended. The argument for writing our own is that Apple's is unmaintained
// and that ALHACK is what unmaintained decoders eventually produce -- an
// argument that would be worth nothing if ours were merely newer rather than
// actually exercised. Every bounds check in alac.cpp is here to be tested by
// this file.
//
// **It used to fuzz the container too**, because a parser here decided where an
// ALAC packet was. `demux_mp4` reads MP4 with Bento4 now and that parser is
// gone; the other side of the same file is `mp4_fuzzer`, over the library that
// took its place.

#include "alac.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    // The first 24 bytes as a magic cookie, the rest as one packet.
    if (size > 24) {
        mp::alac::Config cfg;
        if (mp::alac::parse_config(data, 24, cfg)) {
            // Held across inputs so that the allocation is not the thing being
            // measured. init() re-sizes it whenever the cookie changes.
            static mp::alac::Decoder decoder;
            static std::int32_t out[mp::alac::k_max_frame_length * mp::alac::k_max_channels];
            if (decoder.init(cfg)) {
                decoder.decode(data + 24, size - 24, out);
                // Again from the middle of the buffer: a second packet exercises
                // the paths that assume a frame followed a frame.
                if (size > 64) {
                    decoder.decode(data + 32, size - 32, out);
                }
            }
        }
    }

    return 0;
}

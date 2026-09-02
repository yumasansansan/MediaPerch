// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ALAC container parser and codec, given arbitrary bytes.
//
// This is the fuzzer that had to exist before an ALAC decoder written here could
// be defended. The argument for writing our own is that Apple's is unmaintained
// and that ALHACK is what unmaintained decoders eventually produce -- an
// argument that would be worth nothing if ours were merely newer rather than
// actually exercised. Every bounds check in alac.cpp and mp4.cpp is here to be
// tested by this file.
//
// Two parsers, one input, because they sit on opposite sides of the same file:
// mp4.cpp decides where a packet is, and alac.cpp decides what is in it.

#include "alac.hpp"
#include "mp4.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    // The container: the input as the payload of a `moov` box.
    {
        mp::mp4::AudioTrack track;
        const char* why = "";
        mp::mp4::parse_moov(data, size, track, &why);
    }

    // The codec: the first 24 bytes as a magic cookie, the rest as one packet.
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

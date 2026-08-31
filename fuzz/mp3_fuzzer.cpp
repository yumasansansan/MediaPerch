// SPDX-License-Identifier: GPL-3.0-or-later
//
// dr_mp3, given arbitrary bytes. See wav_fuzzer.cpp for why these exist.
//
// MP3 earns one for a reason the other two do not: the frame parser resynchronises
// after garbage, so it will keep going through a file that is mostly not MP3,
// which is a lot of decoding driven by a lot of untrusted input. The gapless
// path adds to that -- the LAME tag supplies a delay and a padding that become
// skip counts, and skip counts derived from a file are exactly the kind of
// number that wants a bound on it.

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_FLOAT_OUTPUT
#include <dr_mp3.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    drmp3 mp3;
    if (drmp3_init_memory(&mp3, data, size, nullptr) == DRMP3_FALSE) {
        return 0;
    }

    if (mp3.channels != 0 && mp3.channels <= 2) {
        constexpr std::size_t frames_per_read = 1152;
        // Static rather than a vector: see wav_fuzzer.cpp.
        static float buffer[frames_per_read * 2];

        for (int i = 0; i < 8; ++i) {
            if (drmp3_read_pcm_frames_f32(&mp3, frames_per_read, buffer) == 0) {
                break;
            }
        }

        // Seeking re-enters the frame parser from the middle, and for MP3 that
        // also re-runs the gapless skip against a different starting position.
        drmp3_seek_to_pcm_frame(&mp3, 1);
        drmp3_read_pcm_frames_f32(&mp3, frames_per_read, buffer);
    }

    drmp3_uninit(&mp3);
    return 0;
}

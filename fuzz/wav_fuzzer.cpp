// SPDX-License-Identifier: GPL-3.0-or-later
//
// dr_wav, given arbitrary bytes.
//
// This is the parser that reads files other people made, and §2 of the plan
// chose C++ over Rust for it on the argument that libFuzzer and the sanitizers
// close the gap. That argument is only worth anything if the fuzzer exists and
// runs, so it runs in CI on every push rather than by hand once a year.
//
// The implementation is compiled into this binary rather than linked from the
// module, so that ASan instruments dr_wav's own arithmetic and not just ours.

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_CONVERSION_API
#define DRWAV_MAX_SAMPLE_RATE 6144000
#include <dr_wav.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    drwav wav;
    if (!drwav_init_memory(&wav, data, size, nullptr)) {
        return 0;
    }

    // A malformed header can claim 256 channels of 64-bit samples. Reading is
    // still the interesting part -- the frame loop is where the arithmetic is --
    // but the buffer has to be bounded by something other than the file's own
    // claims about itself.
    const std::size_t frame_bytes =
        static_cast<std::size_t>(wav.channels) * (wav.bitsPerSample / 8);
    if (frame_bytes != 0 && frame_bytes <= 4096) {
        constexpr std::size_t frames_per_read = 128;
        // Static rather than a vector: MSVC's STL turns on container
        // annotations under ASan and then disagrees with the runtime about
        // them, and a fuzz target has no business allocating per case anyway.
        static std::uint8_t buffer[frames_per_read * 4096];

        // Bounded: a file that claims four billion frames must not turn one
        // fuzz case into a minute.
        for (int i = 0; i < 8; ++i) {
            if (drwav_read_pcm_frames(&wav, frames_per_read, buffer) == 0) {
                break;
            }
        }

        drwav_seek_to_pcm_frame(&wav, 1);
        drwav_read_pcm_frames(&wav, frames_per_read, buffer);
    }

    drwav_uninit(&wav);
    return 0;
}

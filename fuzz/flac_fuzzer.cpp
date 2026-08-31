// SPDX-License-Identifier: GPL-3.0-or-later
//
// dr_flac, given arbitrary bytes. See wav_fuzzer.cpp for why this exists.
//
// FLAC is the more interesting of the two: a WAV header is a handful of fields,
// while a FLAC frame carries Rice-coded residuals whose partition orders and
// predictor coefficients all come out of the file. It is the parser in this tree
// with real arithmetic in it.

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include <dr_flac.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    drflac* flac = drflac_open_memory(data, size, nullptr);
    if (flac == nullptr) {
        return 0;
    }

    if (flac->channels != 0 && flac->channels <= 8) {
        constexpr std::size_t frames_per_read = 512;
        // Static rather than a vector: see wav_fuzzer.cpp.
        static drflac_int32 buffer[frames_per_read * 8];

        for (int i = 0; i < 8; ++i) {
            if (drflac_read_pcm_frames_s32(flac, frames_per_read, buffer) == 0) {
                break;
            }
        }

        // Seeking re-enters the frame parser from the middle of the stream,
        // which is a different path through the same arithmetic.
        drflac_seek_to_pcm_frame(flac, 1);
        drflac_read_pcm_frames_s32(flac, frames_per_read, buffer);
    }

    drflac_close(flac);
    return 0;
}

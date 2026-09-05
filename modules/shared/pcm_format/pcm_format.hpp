// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a bit depth means, said once.
//
// **This was copied into six modules and had already drifted.** `demux_wav`,
// `demux_mkv`, `demux_flac`, `demux_wavpack`, `demux_mf` and `codec_flac` each
// carried their own `sample_type_for`; five were character-for-character
// identical and the sixth -- `demux_wavpack` -- had lost the `valid == 0` guard
// along the way, so a stream that stated no bit depth got a sample type from
// one module and MP_SAMPLE_NONE from every other. Nothing had noticed, because
// no file this tree reads reaches WavPack without a depth.
//
// The drift that did bite was float. `demux_wav` reported float WAV as
// MP_SAMPLE_F32 and `demux_mkv` refused `A_PCM/FLOAT` outright, on an argument
// -- that `codec_pcm` is a memcpy -- which is true and does not follow, since a
// memcpy is correct for float too. Two modules answering one question two ways
// is what a shared answer is for.
//
// A static library rather than a header of inline functions, because that is
// what modules/shared already is: `biquad`, `transform` and `convolve` are all
// arithmetic two modules would otherwise each own.
//
// **Not in the ABI header.** These are policy, not vocabulary: that 24 bits in
// a four-byte container is MP_SAMPLE_S24_IN_32 rather than MP_SAMPLE_S32 is a
// decision this tree makes about how to describe a file, and a third-party
// module is free to describe one differently.

#ifndef MEDIAPERCH_SHARED_PCM_FORMAT_HPP
#define MEDIAPERCH_SHARED_PCM_FORMAT_HPP

#include <mediaperch/module.h>

#include <cstdint>

namespace mp::pcm {

/// How many bytes a sample of `bits` occupies, or 0 for a depth nothing here
/// carries.
///
/// **Packed, not padded**: 24-bit audio is three bytes here and not four,
/// because that is how every container this tree reads stores it and repacking
/// on the way in would be a conversion nobody asked for. Above 32 bits is
/// nothing this returns a container for -- 64-bit integer PCM does not exist in
/// any format read here, and float has its own function.
[[nodiscard]] std::uint32_t container_for(std::uint32_t bits) noexcept;

/// The sample type for `valid` significant bits inside a `container`-byte slot.
///
/// MP_SAMPLE_NONE for a combination nothing here carries, **including a `valid`
/// of zero**: a stream that does not say how many bits it uses is one this tree
/// declines to guess about, which is the guard `demux_wavpack`'s copy had lost.
[[nodiscard]] MpSampleType sample_type_for(std::uint32_t container,
                                           std::uint32_t valid) noexcept;

/// The sample type for IEEE floating point of `bits`.
///
/// **`valid_bits` is meaningless for float and callers set it to zero.** The
/// field says how many of a container's bits carry the signal, which is a
/// question about integers: a float uses all of its bits and none of them are a
/// magnitude.
[[nodiscard]] MpSampleType float_sample_type(std::uint32_t bits) noexcept;

} // namespace mp::pcm

#endif // MEDIAPERCH_SHARED_PCM_FORMAT_HPP

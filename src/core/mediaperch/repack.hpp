// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <cstddef>

namespace mp {

/// Move integer PCM samples between containers without changing the signal.
///
/// This is the second half of the passthrough path, and the reason `Fidelity`
/// distinguishes `exact` from `repacked`: exact is a `memcpy`, repacked is this.
/// No float, no gain, no rounding, no dither.
///
/// **Samples are left-justified**, which is the convention
/// `WAVEFORMATEXTENSIBLE` states: when `wValidBitsPerSample` is less than
/// `wBitsPerSample`, the valid bits occupy the *most* significant bits and the
/// padding is zeros at the bottom. Getting this backwards does not fail, it just
/// plays about 48 dB too quietly, which is why it has a test of its own.
///
/// Because everything is left-justified, the operation is the same in both
/// directions and needs no arithmetic at all: keep the top `min(from, to)`
/// bytes, and pad the bottom with zeros. Going to a *smaller* container is
/// therefore lossless exactly when the valid bits still fit, which is the only
/// thing the caller has to check -- and `build_candidates` never produces a pair
/// where they do not.
///
/// It was originally called `promote` and only widened, because the first device
/// to hand back a 24-bit format wanted the four-byte one. The second wanted the
/// three-byte one.
///
/// Runs on the decode thread, not the render thread.
///
/// Returns false for a pair that is not a lossless repack, in which case `dst`
/// is untouched. `samples` counts samples, not frames.
bool repack(const void* src, SampleType from, void* dst, SampleType to,
            std::uint32_t valid_bits, std::size_t samples) noexcept;

/// Bytes `repack` will write for `samples` samples of `to`.
[[nodiscard]] std::size_t repacked_bytes(SampleType to, std::size_t samples) noexcept;

} // namespace mp

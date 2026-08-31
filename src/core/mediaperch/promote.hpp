// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <cstddef>

namespace mp {

/// Move integer PCM samples into a wider container without changing the signal.
///
/// This is the second half of the passthrough path, and the reason `Fidelity`
/// distinguishes `exact` from `widened`: exact is a `memcpy`, widened is this.
/// No float, no gain, no rounding, no dither -- a fixed shift and nothing else.
///
/// **Samples are left-justified**, which is the convention
/// `WAVEFORMATEXTENSIBLE` states: when `wValidBitsPerSample` is less than
/// `wBitsPerSample`, the valid bits occupy the *most* significant bits and the
/// padding is zeros at the bottom. Getting this backwards does not fail, it just
/// plays about 48 dB too quietly, which is why it has a test of its own.
///
/// Runs on the decode thread, not the render thread.
///
/// Returns false for a pair that is not a widening, in which case `dst` is
/// untouched. `samples` counts samples, not frames.
bool promote(const void* src, SampleType from, void* dst, SampleType to,
             std::size_t samples) noexcept;

/// Bytes `promote` will write for `samples` samples of `to`.
[[nodiscard]] std::size_t promoted_bytes(SampleType to, std::size_t samples) noexcept;

} // namespace mp

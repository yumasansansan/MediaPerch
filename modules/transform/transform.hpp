// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transforms, and the one thing built on them that more than one module
// wants.
//
// These began inside the resampler, which is where they were first needed. They
// are here because the equaliser needs the same three: a fast transform to
// design a filter with, an any-length one to build a window with, and the
// cepstral factorisation that turns a magnitude response into a filter whose
// energy is at the front of it. Shared the way `modules/mp4` and
// `modules/biquad` are shared -- by the thing neither owner should own.

#ifndef MEDIAPERCH_TRANSFORM_HPP
#define MEDIAPERCH_TRANSFORM_HPP

#include <complex>
#include <cstddef>
#include <vector>

namespace mp::transform {

[[nodiscard]] std::size_t next_power_of_two(std::size_t n) noexcept;

/// In-place radix-2 FFT. `a.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& a, bool inverse);

/// The DFT of any length, via Bluestein's chirp-z. Needed because a window is
/// as long as the filter and filter lengths are 11201 and 25281 rather than
/// 16384.
void dft_any(std::vector<std::complex<double>>& a);

/// Replaces `h` with the minimum-phase filter of the same magnitude response.
///
/// The real cepstrum, folded onto its causal half: a spectrum's magnitude
/// decides its minimum-phase counterpart, and the fold is how that counterpart
/// is computed. `floor_db` is where the logarithm is clamped -- a stopband null
/// is a true zero, and the logarithm of zero has no folded version.
/// `oversample` is how much room the cepstrum gets, as a multiple of the
/// filter's length: too little and its tail wraps onto its head, which comes
/// out as a filter with the right shape and the wrong magnitude.
void to_minimum_phase(std::vector<double>& h, double floor_db, unsigned oversample);

} // namespace mp::transform

#endif // MEDIAPERCH_TRANSFORM_HPP

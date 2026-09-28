// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loudest sample of a block, measured the way every stage that reports a
// peak measures it.
//
// Five stages say what they did by the largest magnitude they wrote or were
// given -- the gain, the equaliser, the convolver, the downmix and the loudness
// meter -- and each had its own copy of the loop, kept scalar the same way in
// all five. It is here once, in the form the compiler makes vectors of.

#ifndef MEDIAPERCH_PEAK_HPP
#define MEDIAPERCH_PEAK_HPP

#include <cmath>
#include <cstddef>

namespace mp::peak {

/// The largest of `peak` and the magnitudes of `x[0]` to `x[count - 1]`, where
/// `peak` is +0.0 or more and a NaN is never the largest.
///
/// **Sixteen running peaks, not one.** One is a chain of comparisons in the
/// order the samples come, which the compiler may not reorder without leave to
/// disregard NaN and the sign of zero, and so kept scalar. Sixteen that each
/// keep their own are sixteen chains it makes whole vectors of -- four of AVX2,
/// two of AVX-512 -- and the answer is the same number: a lane starts at +0.0
/// and takes only a magnitude greater than its own, so none is ever NaN or
/// -0.0, and the largest of such numbers does not depend on the order they are
/// compared in.
[[nodiscard]] inline double loudest(const double* x, std::size_t count, double peak) noexcept
{
    constexpr std::size_t k_lanes = 16;
    double lanes[k_lanes] = {};
    std::size_t n = 0;
    for (; n + k_lanes <= count; n += k_lanes) {
        for (std::size_t j = 0; j < k_lanes; ++j) {
            const double magnitude = std::fabs(x[n + j]);
            lanes[j] = magnitude > lanes[j] ? magnitude : lanes[j];
        }
    }
    for (; n < count; ++n) {
        const double magnitude = std::fabs(x[n]);
        peak = magnitude > peak ? magnitude : peak;
    }
    for (const double lane : lanes) {
        peak = lane > peak ? lane : peak;
    }
    return peak;
}

} // namespace mp::peak

#endif

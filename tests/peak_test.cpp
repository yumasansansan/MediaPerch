// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loudest sample, held to the loop it replaced.
//
// modules/shared/peak keeps sixteen running peaks so that the compiler can make
// vectors of them, and claims the very number one running peak gives, NaN and
// the sign of zero included. So the one running peak is written out here as
// every stage had it, and the two are compared bit for bit: at every length up
// to a few vectors and a tail, with the loudest sample in every place, and
// from peaks carried in from before.

#include <peak.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

/// The loop the gain, the equaliser, the convolver, the downmix and the
/// loudness meter each had.
double one_running_peak(const double* x, std::size_t count, double peak)
{
    for (std::size_t n = 0; n < count; ++n) {
        const double magnitude = x[n] < 0.0 ? -x[n] : x[n];
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

} // namespace

TEST_CASE("the loudest sample is the one a single running peak finds", "[peak]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    std::size_t compared = 0;
    std::size_t differed = 0;
    for (std::size_t count = 0; count <= 70; ++count) {
        for (std::size_t loudest = 0; loudest < std::max<std::size_t>(count, 1); ++loudest) {
            // Quiet samples of both signs, with NaNs and negative zeros among
            // them, and one loud one.
            std::vector<double> x(count);
            for (std::size_t n = 0; n < count; ++n) {
                x[n] = static_cast<double>(n % 5) * (n % 2 == 0 ? 0.05 : -0.05);
                if (n % 7 == 3) {
                    x[n] = nan;
                } else if (n % 11 == 5) {
                    x[n] = -0.0;
                }
            }
            if (count > 0) {
                x[loudest] = loudest % 2 == 0 ? 0.75 : -0.75;
            }
            for (const double before : {0.0, 0.5, 1.0}) {
                const double want = one_running_peak(x.data(), count, before);
                const double got = mp::peak::loudest(x.data(), count, before);
                if (std::bit_cast<std::uint64_t>(got) != std::bit_cast<std::uint64_t>(want)) {
                    ++differed;
                    UNSCOPED_INFO(count << " samples, loudest at " << loudest << ", from "
                                        << before << ": " << got << " for " << want);
                }
                ++compared;
            }
        }
    }
    CHECK(compared > 7000);
    CHECK(differed == 0);

    // Nothing but NaN leaves the peak where it was, and an infinity is the
    // loudest there is, in a lane or in the tail.
    const std::vector<double> nothing(40, nan);
    CHECK(std::bit_cast<std::uint64_t>(mp::peak::loudest(nothing.data(), nothing.size(), 0.0)) ==
          std::bit_cast<std::uint64_t>(0.0));
    for (const std::size_t at : {std::size_t{3}, std::size_t{35}}) {
        std::vector<double> x(36, 0.5);
        x[at] = -infinity;
        CHECK(mp::peak::loudest(x.data(), x.size(), 0.0) == infinity);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/framerate.hpp"

#include <cmath>

namespace mp {

std::span<const Rational> standard_frame_rates() noexcept
{
    // Every rate a container in this tree has been seen to carry, with the
    // 1000/1001 partner of each. Ordered by rate, so the spacing claim in the
    // header can be checked by looking: the neighbours that are close are the
    // pairs, and they are a thousandth apart.
    static constexpr Rational k_rates[] = {
        {25, 2},        {15, 1},   {24000, 1001}, {24, 1},        {25, 1},
        {30000, 1001},  {30, 1},   {48000, 1001}, {48, 1},        {50, 1},
        {60000, 1001},  {60, 1},   {100, 1},      {120000, 1001}, {120, 1},
    };
    return {k_rates, sizeof(k_rates) / sizeof(k_rates[0])};
}

Rational snap_frame_rate(Rational stated, bool* snapped) noexcept
{
    if (snapped != nullptr) {
        *snapped = false;
    }
    if (!stated.valid()) {
        return stated;
    }
    // An exact match is not a rounding of anything, and saying so first keeps
    // `snapped` honest for the containers that can state a ratio.
    for (const Rational& candidate : standard_frame_rates()) {
        if (candidate == stated) {
            return stated;
        }
    }
    const double have = stated.hz();
    for (const Rational& candidate : standard_frame_rates()) {
        const double want = candidate.hz();
        if (want > 0.0 && std::abs(have - want) / want < 1e-6) {
            if (snapped != nullptr) {
                *snapped = true;
            }
            return candidate;
        }
    }
    return stated;
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/refresh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace mp {
namespace {

/// `refresh / fps`, in halves, as an exact fraction.
///
/// Doubling is what lets one remainder answer both questions: `2 * r` whole and
/// even is a whole number of refreshes per frame, whole and odd is a half, and
/// anything else is neither. The alternative is two divisions and a tolerance,
/// and a tolerance is what this file exists to avoid.
struct Halves {
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
};

Halves halves(const Rational& refresh, const Rational& fps)
{
    // 2 * (rn/rd) / (fn/fd) = 2 * rn * fd / (rd * fn). Four 32-bit numbers make
    // at most 64 bits when multiplied in pairs, and the pairs are what this is.
    Halves out;
    out.numerator = 2ull * refresh.num * fps.den;
    out.denominator = static_cast<std::uint64_t>(refresh.den) * fps.num;
    const std::uint64_t common = std::gcd(out.numerator, out.denominator);
    if (common != 0) {
        out.numerator /= common;
        out.denominator /= common;
    }
    return out;
}

} // namespace

std::vector<Match> rank_modes(std::span<const DisplayMode> modes, Rational fps,
                              std::uint32_t width, std::uint32_t height, Prefer prefer)
{
    std::vector<Match> out;
    if (!fps.valid()) {
        return out;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const DisplayMode& mode = modes[i];
        if (mode.width != width || mode.height != height || !mode.refresh.valid()) {
            continue;
        }
        Match m;
        m.index = i;
        m.mode = mode;
        m.refreshes_per_frame = mode.refresh.hz() / fps.hz();

        const Halves h = halves(mode.refresh, fps);
        // The nearest repeating pattern, rounded in the doubled units so that
        // "nearest half" needs no floating point either.
        const std::uint64_t rounded =
            (h.numerator + h.denominator / 2) / (h.denominator != 0 ? h.denominator : 1);
        m.nearest = static_cast<double>(rounded) / 2.0;
        m.exact = h.denominator != 0 && h.numerator % h.denominator == 0;

        if (m.refreshes_per_frame < 1.0) {
            // Below the frame rate. Reported rather than dropped from the list,
            // because a caller with nothing better should be told what it has.
            m.cadence = Cadence::drops;
        } else if (rounded % 2 == 0) {
            m.cadence = Cadence::even;
        } else {
            m.cadence = Cadence::pulldown;
        }

        if (m.exact) {
            m.seconds_between_slips = std::numeric_limits<double>::infinity();
        } else {
            // How many frames until the phase has walked a whole refresh, then
            // how long that is. `nearest` is the pattern it is walking from.
            const double drift = std::abs(m.refreshes_per_frame - m.nearest);
            m.seconds_between_slips = drift > 0.0 ? 1.0 / (drift * fps.hz())
                                                  : std::numeric_limits<double>::infinity();
        }
        out.push_back(m);
    }

    // Even before pulldown, exact before near, and then whichever way the
    // caller asked -- the first two are about the film and the third is not.
    std::sort(out.begin(), out.end(), [prefer](const Match& a, const Match& b) {
        if (a.cadence != b.cadence) {
            return a.cadence > b.cadence;
        }
        if (a.exact != b.exact) {
            return a.exact;
        }
        if (!a.exact && a.seconds_between_slips != b.seconds_between_slips) {
            return a.seconds_between_slips > b.seconds_between_slips;
        }
        return prefer == Prefer::fastest ? a.mode.refresh.hz() > b.mode.refresh.hz()
                                         : a.mode.refresh.hz() < b.mode.refresh.hz();
    });
    return out;
}

std::optional<Match> best_mode(std::span<const DisplayMode> modes, Rational fps,
                               std::uint32_t width, std::uint32_t height, Prefer prefer)
{
    const std::vector<Match> ranked = rank_modes(modes, fps, width, height, prefer);
    if (ranked.empty()) {
        return std::nullopt;
    }
    return ranked.front();
}

bool prefer_from_name(std::string_view name, Prefer& out) noexcept
{
    if (name == "fastest") {
        out = Prefer::fastest;
        return true;
    }
    if (name == "slowest") {
        out = Prefer::slowest;
        return true;
    }
    return false;
}

std::string describe(const Match& match)
{
    char line[192];
    const char* kind = match.cadence == Cadence::even      ? "even"
                       : match.cadence == Cadence::pulldown ? "pulldown"
                                                            : "drops frames";
    if (match.exact) {
        std::snprintf(line, sizeof(line), "%.3f Hz, %.1f refreshes per frame exactly (%s)",
                      match.mode.refresh.hz(), match.nearest, kind);
    } else {
        std::snprintf(line, sizeof(line),
                      "%.3f Hz, %.4f refreshes per frame (%s %.1f, slips every %.1f s)",
                      match.mode.refresh.hz(), match.refreshes_per_frame, kind,
                      match.nearest, match.seconds_between_slips);
    }
    return line;
}

} // namespace mp

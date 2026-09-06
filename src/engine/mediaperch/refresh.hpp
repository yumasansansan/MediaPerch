// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which display mode suits a frame rate, decided in exact arithmetic.
//
// **This is the first of the three things that make the error smaller**, and
// §8's "why it is not zero" names it: a refresh that is a whole multiple of the
// frame rate has nowhere to put a frame except the right place. The other two
// are variable refresh and knowing the appearance instant, and neither is here.
//
// Nothing in this file touches a display. It takes a list of modes and a frame
// rate and says which mode, which is what makes it testable on a machine with
// one 60 Hz panel -- the modes are a table, and the answers are arithmetic.
//
// **The arithmetic is exact and that is the whole point.** 24000/1001 against
// 60000/1001 is five halves and against 48000/1001 is two, and in `double`
// neither of those is quite true. A ratio is two integers here, and "does this
// divide" is a remainder rather than a tolerance.
//
// Three things a caller needs to know about a candidate, and they are not the
// same thing:
//
//  * **The cadence.** A whole number of refreshes per frame shows every frame
//    for the same length of time. A half -- 2.5, which is 3:2 pulldown -- shows
//    them alternately for two and three, which is judder, and is what 23.976 on
//    a 60 Hz panel has always been.
//  * **Whether it holds.** A cadence that is exact repeats forever. One that is
//    near-exact slips a refresh every so often, and that slip is a visible
//    hitch. Measured on this tree's panel: 24p on 60.000 Hz breaks every 16.7
//    seconds, and on 59.940 never.
//  * **Whether frames survive.** A refresh below the frame rate cannot show
//    them all, whatever the ratio.
//
// The first two are separate axes and a mode can be good at one and bad at the
// other, which is why `Match` reports both rather than one score. Measured:
// 59.940 Hz is an *exact* 2.5 for 24p and still puts every other frame half a
// refresh out, because half a refresh is what a 3:2 cadence is.

#ifndef MEDIAPERCH_REFRESH_HPP
#define MEDIAPERCH_REFRESH_HPP

#include "mediaperch/rational.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// One entry of a display's mode list.
struct DisplayMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Rational refresh;
};

/// What a mode does with a frame rate.
enum class Cadence : std::uint32_t {
    /// Fewer refreshes than frames: some frames cannot be shown at all.
    drops,
    /// A half-integer ratio. 3:2 pulldown and its relatives -- every frame is
    /// shown, alternately for different lengths of time.
    pulldown,
    /// A whole number of refreshes per frame. Nothing alternates.
    even,
};

/// Which of two equally correct modes to take.
///
/// **23.976 and 47.952 are both exactly right for a 23.976 fps film**, one
/// refresh per frame and two, and nothing about the video distinguishes them.
/// What differs is everything else: at the slower one the cursor, the subtitles
/// and any overlay move at 24 Hz too, and a panel asked to run that slowly may
/// flicker or refuse. At the faster one the display repeats each frame and
/// spends the power to do it.
///
/// `fastest` is the default, and the reason is measured rather than assumed:
/// this tree's panel would not show a 23.976 Hz mode at all until its timing
/// was rebuilt with the vertical total raised, and even now that mode sits just
/// above a line rate the panel refuses. Choosing the slowest exact multiple by
/// default would put a player on the least reliable mode a display has.
enum class Prefer : std::uint32_t {
    /// The highest exact multiple. Smoother for everything that is not the
    /// film, and the mode a display is likeliest to be happy with.
    fastest,
    /// The lowest. One refresh per frame if the display has it -- less power,
    /// and what a television does with a 24 Hz mode.
    slowest,
};

/// One candidate, measured rather than ranked.
struct Match {
    /// Where it came from in the list handed in.
    std::size_t index = 0;
    DisplayMode mode;
    Cadence cadence = Cadence::drops;
    /// Refreshes per frame, as a number for reporting. The decision is made in
    /// integers; this is for saying so.
    double refreshes_per_frame = 0.0;
    /// The repeating pattern this is nearest to, in halves of a refresh: 2.0 is
    /// even, 2.5 is 3:2.
    double nearest = 0.0;
    /// True when the ratio is that pattern exactly, so the cadence never
    /// breaks. Decided by a remainder and not by a tolerance.
    bool exact = false;
    /// How long the cadence holds before it slips one refresh, in seconds.
    /// Infinite when `exact`.
    double seconds_between_slips = 0.0;
};

/// Every mode in `modes` that is `width` x `height`, measured against `fps`.
///
/// The size is a filter rather than a preference: switching resolution to gain
/// a refresh rate would trade a real thing for a smaller one, and the caller
/// has a desktop that is a particular size for a reason.
[[nodiscard]] std::vector<Match> rank_modes(std::span<const DisplayMode> modes,
                                            Rational fps, std::uint32_t width,
                                            std::uint32_t height,
                                            Prefer prefer = Prefer::fastest);

/// The best of them, or nothing when the list has none of that size.
///
/// **Even beats pulldown, exact beats near, and then `prefer` decides.** The
/// first is the judder and the second is the hitch; the third is not about the
/// film at all, which is why it is the caller's. A caller that wants the
/// current mode left alone when the gain is small compares this with the
/// current mode itself -- there is no opinion here about whether a switch is
/// worth making.
[[nodiscard]] std::optional<Match> best_mode(std::span<const DisplayMode> modes,
                                             Rational fps, std::uint32_t width,
                                             std::uint32_t height,
                                             Prefer prefer = Prefer::fastest);

/// `fastest` or `slowest`. False for anything else.
[[nodiscard]] bool prefer_from_name(std::string_view name, Prefer& out) noexcept;

/// One line about a match, for a program that has to say what it chose.
[[nodiscard]] std::string describe(const Match& match);

} // namespace mp

#endif // MEDIAPERCH_REFRESH_HPP

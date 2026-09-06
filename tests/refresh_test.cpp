// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which mode suits which frame rate.
//
// The modes below are the nine this tree's panel actually has, written down as
// a table, which is the point of the policy being arithmetic: a machine with
// one 60 Hz panel can check every answer. The three at the top of each list are
// the ones the panel came with, before an EDID override put the other six
// there -- so the questions the file asks are the ones a real display raises.

#include "mediaperch/refresh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

constexpr std::uint32_t k_w = 1920;
constexpr std::uint32_t k_h = 1080;

mp::DisplayMode at(std::uint32_t num, std::uint32_t den)
{
    return mp::DisplayMode{k_w, k_h, mp::Rational{num, den}};
}

/// What this panel offered before anything was added to it.
std::vector<mp::DisplayMode> factory()
{
    return {at(6932000, 231088), at(13850000, 231088), at(13866000, 231088)};
}

/// And after: nine modes, each exact.
std::vector<mp::DisplayMode> extended()
{
    return {at(30, 1),        at(30000, 1001), at(60, 1),       at(60000, 1001),
            at(50, 1),        at(48, 1),       at(48000, 1001), at(24, 1),
            at(24000, 1001)};
}

const mp::Rational k_film{24000, 1001}; // 23.976
const mp::Rational k_cinema{24, 1};
const mp::Rational k_pal{25, 1};
const mp::Rational k_ntsc{30000, 1001}; // 29.97
const mp::Rational k_sixty{60, 1};

} // namespace

TEST_CASE("an exact ratio is decided by a remainder, not a tolerance", "[refresh]")
{
    // **23.976 into 47.952 is two, and in double it is not.** 48000/1001
    // divided by 24000/1001 is exactly 2, and the same numbers as doubles are
    // 47.952047952047955 and 23.976023976023978, whose quotient is 2 only if
    // you do not look. This is why the ratio is two integers here.
    const auto modes = extended();
    const auto ranked = mp::rank_modes(modes, k_film, k_w, k_h);
    REQUIRE(!ranked.empty());

    const auto exact_at = [&](double hz) {
        for (const mp::Match& m : ranked) {
            if (std::abs(m.mode.refresh.hz() - hz) < 0.001) {
                return m;
            }
        }
        FAIL("no mode near " << hz);
        return ranked.front();
    };

    // Two exactly, and it never slips.
    const mp::Match even = exact_at(47.952);
    CHECK(even.cadence == mp::Cadence::even);
    CHECK(even.exact);
    CHECK(even.nearest == Catch::Approx(2.0));
    CHECK(std::isinf(even.seconds_between_slips));

    // Five halves exactly: 3:2 pulldown that never breaks. **Exact and still
    // judder** -- the two axes are not the same axis.
    const mp::Match five_halves = exact_at(59.94);
    CHECK(five_halves.cadence == mp::Cadence::pulldown);
    CHECK(five_halves.exact);
    CHECK(five_halves.nearest == Catch::Approx(2.5));

    // Near five halves: the same judder, and a break as well.
    const mp::Match nearly = exact_at(60.0);
    CHECK(nearly.cadence == mp::Cadence::pulldown);
    CHECK_FALSE(nearly.exact);
    CHECK(nearly.seconds_between_slips == Catch::Approx(16.7).margin(0.5));

    // And 24.000 for 23.976 content: even, but it walks. 41.7 seconds and not
    // the 20.9 that 48.000 gets, because the drift is per frame and the ratio
    // is 1.001 rather than 2.002 -- **a slower mode walks at half the speed**,
    // which is a reason to prefer one that is not about the cadence at all.
    const mp::Match wrong_24 = exact_at(24.0);
    CHECK(wrong_24.cadence == mp::Cadence::even);
    CHECK_FALSE(wrong_24.exact);
    CHECK(wrong_24.seconds_between_slips == Catch::Approx(41.7).margin(0.5));
    CHECK(exact_at(48.0).seconds_between_slips == Catch::Approx(20.9).margin(0.5));
}

TEST_CASE("even beats pulldown, and exact beats near", "[refresh]")
{
    const auto modes = extended();

    // Film gets an exact whole multiple: not the exact 3:2 at 59.94, and not
    // the 60.000 that Windows would have left it on.
    const auto film = mp::best_mode(modes, k_film, k_w, k_h);
    REQUIRE(film);
    CHECK(film->cadence == mp::Cadence::even);
    CHECK(film->exact);
    CHECK(film->mode.refresh == mp::Rational{48000, 1001});

    // True 24 gets the other 48, and the two are a thousandth apart.
    const auto cinema = mp::best_mode(modes, k_cinema, k_w, k_h);
    REQUIRE(cinema);
    CHECK(cinema->mode.refresh == mp::Rational{48, 1});
    CHECK(cinema->exact);

    // 25 fps gets 50, which is the mode that exists for it.
    const auto pal = mp::best_mode(modes, k_pal, k_w, k_h);
    REQUIRE(pal);
    CHECK(pal->mode.refresh == mp::Rational{50, 1});
    CHECK(pal->exact);

    // 29.97 gets 59.94, which is two refreshes a frame, over the 29.97 that is
    // one. Both are exact and even and the film cannot tell them apart.
    const auto ntsc = mp::best_mode(modes, k_ntsc, k_w, k_h);
    REQUIRE(ntsc);
    CHECK(ntsc->mode.refresh == mp::Rational{60000, 1001});
    CHECK(ntsc->exact);
    CHECK(ntsc->nearest == Catch::Approx(2.0));

    // 60 fps cannot use any of the slow ones: a refresh under the frame rate
    // drops frames, and the ranking says so rather than choosing it.
    const auto sixty = mp::best_mode(modes, k_sixty, k_w, k_h);
    REQUIRE(sixty);
    CHECK(sixty->cadence == mp::Cadence::even);
    CHECK(sixty->mode.refresh.hz() >= 60.0);
}

TEST_CASE("which of two equally right modes is the caller's", "[refresh]")
{
    // **23.976 and 47.952 are both exactly right for a 23.976 fps film.**
    // Nothing about the film distinguishes them, so the tie-break is a setting
    // rather than an opinion, and both answers are here because both are right.
    const auto modes = extended();

    const auto fast = mp::best_mode(modes, k_film, k_w, k_h, mp::Prefer::fastest);
    REQUIRE(fast);
    CHECK(fast->mode.refresh == mp::Rational{48000, 1001});
    CHECK(fast->exact);
    CHECK(fast->nearest == Catch::Approx(2.0));

    const auto slow = mp::best_mode(modes, k_film, k_w, k_h, mp::Prefer::slowest);
    REQUIRE(slow);
    CHECK(slow->mode.refresh == mp::Rational{24000, 1001});
    CHECK(slow->exact);
    CHECK(slow->nearest == Catch::Approx(1.0));

    // The tie-break moves nothing that is not a tie: an exact whole multiple
    // still beats an exact pulldown whichever way it is asked.
    for (const mp::Prefer prefer : {mp::Prefer::fastest, mp::Prefer::slowest}) {
        const auto any = mp::best_mode(modes, k_film, k_w, k_h, prefer);
        REQUIRE(any);
        CHECK(any->cadence == mp::Cadence::even);
        CHECK(any->exact);
    }

    // And 29.97 the other way round is the one refresh a frame.
    const auto ntsc_slow = mp::best_mode(modes, k_ntsc, k_w, k_h, mp::Prefer::slowest);
    REQUIRE(ntsc_slow);
    CHECK(ntsc_slow->mode.refresh == mp::Rational{30000, 1001});

    mp::Prefer named = mp::Prefer::slowest;
    CHECK(mp::prefer_from_name("fastest", named));
    CHECK(named == mp::Prefer::fastest);
    CHECK(mp::prefer_from_name("slowest", named));
    CHECK(named == mp::Prefer::slowest);
    CHECK_FALSE(mp::prefer_from_name("quickest", named));
    CHECK(named == mp::Prefer::slowest); // untouched by a name it did not know
}

TEST_CASE("a display with nothing suitable says what it has", "[refresh]")
{
    // The panel as it shipped: 29.997, 59.934, 60.003 and nothing else.
    const auto modes = factory();

    const auto film = mp::best_mode(modes, k_film, k_w, k_h);
    REQUIRE(film);
    // Nothing divides, so the best available is a pulldown -- and the one it
    // picks is 59.934, whose ratio is 2.49974, over the 60.003 whose 2.50263 is
    // ten times further out. **A tenth of a hertz is two minutes of cadence.**
    CHECK(film->cadence == mp::Cadence::pulldown);
    CHECK_FALSE(film->exact);
    CHECK(film->mode.refresh.hz() == Catch::Approx(59.934).margin(0.001));
    CHECK(film->seconds_between_slips == Catch::Approx(161.8).margin(2.0));

    // And 25 fps has no answer here at all: every mode slips inside a second,
    // which is what "this display needs a mode it does not have" looks like.
    const auto pal = mp::best_mode(modes, k_pal, k_w, k_h);
    REQUIRE(pal);
    CHECK(pal->seconds_between_slips < 1.0);
}

TEST_CASE("a mode list of another size is not an answer", "[refresh]")
{
    // Switching resolution to gain a refresh rate trades a real thing for a
    // smaller one, so the size is a filter rather than a preference.
    std::vector<mp::DisplayMode> other{
        mp::DisplayMode{1280, 720, mp::Rational{48000, 1001}},
        mp::DisplayMode{3840, 2160, mp::Rational{24000, 1001}},
    };
    CHECK_FALSE(mp::best_mode(other, k_film, k_w, k_h).has_value());
    CHECK(mp::rank_modes(other, k_film, k_w, k_h).empty());

    // And a frame rate the container would not state is not a question.
    CHECK_FALSE(mp::best_mode(extended(), mp::Rational{0, 0}, k_w, k_h).has_value());
}

TEST_CASE("what it chose can be said in one line", "[refresh]")
{
    const auto film = mp::best_mode(extended(), k_film, k_w, k_h);
    REQUIRE(film);
    const std::string line = mp::describe(*film);
    INFO(line);
    CHECK(line.find("47.952") != std::string::npos);
    CHECK(line.find("2.0 refreshes") != std::string::npos);
    CHECK(line.find("exactly") != std::string::npos);
    CHECK(line.find("even") != std::string::npos);
}

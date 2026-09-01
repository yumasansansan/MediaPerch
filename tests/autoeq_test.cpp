// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading AutoEq's files.
//
// The samples here are in the shape AutoEq publishes -- Equalizer APO's two
// formats -- and what is checked is that every number arrives where it was
// written, including the ones that are easy to lose: the preamp, a filter
// somebody switched off, and the fact that a graphic curve is a *target* rather
// than a cascade.

#include <autoeq.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

const char* k_parametric =
    "Preamp: -6.8 dB\n"
    "Filter 1: ON LSC Fc 105 Hz Gain 4.2 dB Q 0.70\n"
    "Filter 2: ON PK Fc 1058 Hz Gain -1.4 dB Q 1.51\n"
    "Filter 3: OFF PK Fc 8000 Hz Gain 1.0 dB Q 1.00\n"
    "Filter 4: ON HSC Fc 10000 Hz Gain 2.1 dB Q 0.70\n";

} // namespace

TEST_CASE("an AutoEq parametric profile arrives intact", "[autoeq]")
{
    mp::autoeq::Profile profile;
    std::string why;
    INFO(why);
    REQUIRE(mp::autoeq::parse(k_parametric, profile, why));
    CHECK(profile.kind == "parametric");

    // The preamp is part of the profile, not a suggestion: AutoEq computes it
    // so the correction does not clip, and dropping it is how a correction
    // clips.
    CHECK(profile.preamp_db == Catch::Approx(-6.8));
    REQUIRE(profile.bands.size() == 4);

    CHECK(profile.bands[0].kind == mp::biquad::Kind::lowshelf);
    CHECK(profile.bands[0].frequency_hz == Catch::Approx(105.0));
    CHECK(profile.bands[0].gain_db == Catch::Approx(4.2));
    CHECK(profile.bands[0].q == Catch::Approx(0.70));
    CHECK(profile.bands[0].enabled);

    CHECK(profile.bands[1].kind == mp::biquad::Kind::peak);
    CHECK(profile.bands[1].gain_db == Catch::Approx(-1.4));

    // OFF is kept and switched off, rather than dropped: a profile that comes
    // back shorter than it went in is a profile somebody has to reconstruct.
    CHECK_FALSE(profile.bands[2].enabled);
    CHECK(profile.bands[2].frequency_hz == Catch::Approx(8000.0));

    CHECK(profile.bands[3].kind == mp::biquad::Kind::highshelf);
    CHECK(profile.bands[3].frequency_hz == Catch::Approx(10000.0));
}

TEST_CASE("a GraphicEQ profile is a curve, not a cascade", "[autoeq]")
{
    mp::autoeq::Profile profile;
    std::string why;
    REQUIRE(mp::autoeq::parse("GraphicEQ: 20 -1.2; 100 0.0; 1000 3.0; 10000 -2.0",
                              profile, why));
    CHECK(profile.kind == "graphic");
    CHECK(profile.bands.empty());
    REQUIRE(profile.curve.size() == 4);
    CHECK(profile.curve[0].first == Catch::Approx(20.0));
    CHECK(profile.curve[0].second == Catch::Approx(-1.2));

    // Read at its own points it is itself...
    CHECK(mp::autoeq::curve_db(profile.curve, 1000.0) == Catch::Approx(3.0));
    // ...between them it is interpolated on a logarithmic axis, which is the
    // axis it was sampled on...
    const double middle = std::sqrt(100.0 * 1000.0); // half way, logarithmically
    CHECK(mp::autoeq::curve_db(profile.curve, middle) == Catch::Approx(1.5).margin(1e-9));
    // ...and outside its range it holds, because a correction curve says
    // nothing about what it did not measure.
    CHECK(mp::autoeq::curve_db(profile.curve, 5.0) == Catch::Approx(-1.2));
    CHECK(mp::autoeq::curve_db(profile.curve, 40000.0) == Catch::Approx(-2.0));
}

TEST_CASE("the filter types Equalizer APO writes are the ones this reads",
          "[autoeq]")
{
    mp::autoeq::Profile profile;
    std::string why;
    REQUIRE(mp::autoeq::parse("Filter 1: ON PK Fc 1000 Hz Gain 1 dB Q 1\n"
                              "Filter 2: ON LS Fc 100 Hz Gain 1 dB Q 1\n"
                              "Filter 3: ON HS Fc 100 Hz Gain 1 dB Q 1\n"
                              "Filter 4: ON LP Fc 100 Hz Q 1\n"
                              "Filter 5: ON HP Fc 100 Hz Q 1\n"
                              "Filter 6: ON NO Fc 100 Hz Q 1\n"
                              "Filter 7: ON AP Fc 100 Hz Q 1\n"
                              "Filter 8: ON None\n",
                              profile, why));
    INFO(why);
    REQUIRE(profile.bands.size() == 7); // `None` is an empty slot, not a filter
    CHECK(profile.bands[3].kind == mp::biquad::Kind::lowpass);
    CHECK(profile.bands[6].kind == mp::biquad::Kind::allpass);
}

TEST_CASE("a file that is not a profile is refused with the reason", "[autoeq]")
{
    mp::autoeq::Profile profile;
    std::string why;
    CHECK_FALSE(mp::autoeq::parse("hello\n", profile, why));
    CHECK(why.find("not a profile") != std::string::npos);

    CHECK_FALSE(mp::autoeq::parse("Filter 1: ON WOBBLE Fc 100 Hz\n", profile, why));
    CHECK(why.find("WOBBLE") != std::string::npos);

    CHECK_FALSE(mp::autoeq::load("no-such-file-anywhere.txt", profile, why));
    CHECK(why.find("could not open") != std::string::npos);
}

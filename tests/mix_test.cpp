// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel matrix, read rather than listened to.
//
// A downmix cannot be checked by comparing it with anything -- there is no
// correct 5.1-to-stereo, only a defensible one. So what is checked here is that
// the matrix says what the settings said it would, that nothing appears in a
// channel nothing feeds, and that the two normalisations do the two things they
// claim.

#include <mix.hpp>

#include <mediaperch/module.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

constexpr std::uint32_t k_stereo = MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT;
constexpr std::uint32_t k_mono = MP_SPEAKER_FRONT_CENTER;

mp::mix::Matrix built(std::uint32_t in_channels, std::uint32_t out_channels,
                      const mp::mix::Recipe& recipe = {}, std::uint32_t in_mask = 0,
                      std::uint32_t out_mask = 0)
{
    mp::mix::Matrix matrix;
    std::string why;
    const bool ok =
        mp::mix::build(in_channels, in_mask, out_channels, out_mask, recipe, matrix, why);
    INFO(why);
    REQUIRE(ok);
    return matrix;
}

/// Where a speaker sits in a frame, which is the order of the mask's bits.
std::uint32_t index_of(std::uint32_t mask, std::uint32_t speaker)
{
    std::uint32_t at = 0;
    for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((mask & bit) == 0) {
            continue;
        }
        if (bit == speaker) {
            return at;
        }
        ++at;
    }
    FAIL("that speaker is not in that mask");
    return 0;
}

} // namespace

TEST_CASE("the same layout at both ends is the identity", "[mix]")
{
    for (const std::uint32_t channels : {1u, 2u, 4u, 6u, 8u}) {
        const auto matrix = built(channels, channels);
        INFO(channels << " channels: " << matrix.text());
        CHECK(matrix.identity());
    }
}

TEST_CASE("5.1 to stereo puts each thing where the settings said", "[mix]")
{
    mp::mix::Recipe recipe;
    recipe.normalise = mp::mix::Normalise::none; // read the coefficients, not the level
    const auto matrix = built(6, 2, recipe);

    const std::uint32_t in_mask = mp::mix::conventional_mask(6);
    const std::uint32_t left = index_of(k_stereo, MP_SPEAKER_FRONT_LEFT);
    const std::uint32_t right = index_of(k_stereo, MP_SPEAKER_FRONT_RIGHT);
    const double half_power = std::pow(10.0, -3.0 / 20.0);

    // The front pair, straight through.
    CHECK(matrix.at(left, index_of(in_mask, MP_SPEAKER_FRONT_LEFT)) == Catch::Approx(1.0));
    CHECK(matrix.at(right, index_of(in_mask, MP_SPEAKER_FRONT_RIGHT)) ==
          Catch::Approx(1.0));
    CHECK(matrix.at(left, index_of(in_mask, MP_SPEAKER_FRONT_RIGHT)) == 0.0);

    // The centre into both, at the -3 dB the convention asks for.
    CHECK(matrix.at(left, index_of(in_mask, MP_SPEAKER_FRONT_CENTER)) ==
          Catch::Approx(half_power));
    CHECK(matrix.at(right, index_of(in_mask, MP_SPEAKER_FRONT_CENTER)) ==
          Catch::Approx(half_power));

    // The surrounds, each into the front on its own side and nowhere else.
    CHECK(matrix.at(left, index_of(in_mask, MP_SPEAKER_SIDE_LEFT)) ==
          Catch::Approx(half_power));
    CHECK(matrix.at(right, index_of(in_mask, MP_SPEAKER_SIDE_LEFT)) == 0.0);
    CHECK(matrix.at(right, index_of(in_mask, MP_SPEAKER_SIDE_RIGHT)) ==
          Catch::Approx(half_power));

    // And the effects channel is dropped, which is the default and is a
    // decision rather than an oversight.
    CHECK(matrix.at(left, index_of(in_mask, MP_SPEAKER_LOW_FREQUENCY)) == 0.0);
    CHECK(matrix.at(right, index_of(in_mask, MP_SPEAKER_LOW_FREQUENCY)) == 0.0);
}

TEST_CASE("the effects channel comes back when it is asked for", "[mix]")
{
    mp::mix::Recipe recipe;
    recipe.normalise = mp::mix::Normalise::none;
    recipe.lfe_db = -6.0;
    const auto matrix = built(6, 2, recipe);
    const std::uint32_t in_mask = mp::mix::conventional_mask(6);
    CHECK(matrix.at(0, index_of(in_mask, MP_SPEAKER_LOW_FREQUENCY)) ==
          Catch::Approx(std::pow(10.0, -6.0 / 20.0)));
}

TEST_CASE("an upmix places what exists and invents nothing", "[mix]")
{
    // Stereo into 5.1: left in the left, right in the right, and silence in the
    // centre, the surrounds and the effects channel. Anything else would be
    // this program deciding what a recording should have contained.
    const auto matrix = built(2, 6);
    const std::uint32_t out_mask = mp::mix::conventional_mask(6);

    CHECK(matrix.at(index_of(out_mask, MP_SPEAKER_FRONT_LEFT), 0) == Catch::Approx(1.0));
    CHECK(matrix.at(index_of(out_mask, MP_SPEAKER_FRONT_RIGHT), 1) == Catch::Approx(1.0));
    for (const std::uint32_t silent : {MP_SPEAKER_FRONT_CENTER, MP_SPEAKER_LOW_FREQUENCY,
                                       MP_SPEAKER_SIDE_LEFT, MP_SPEAKER_SIDE_RIGHT}) {
        const std::uint32_t row = index_of(out_mask, silent);
        INFO("speaker 0x" << std::hex << silent);
        CHECK(matrix.at(row, 0) == 0.0);
        CHECK(matrix.at(row, 1) == 0.0);
    }
}

TEST_CASE("synthesise is what fills those channels, and it says so", "[mix]")
{
    mp::mix::Recipe recipe;
    recipe.synthesise = true;
    recipe.normalise = mp::mix::Normalise::none;
    const auto matrix = built(2, 6, recipe);
    const std::uint32_t out_mask = mp::mix::conventional_mask(6);
    const double half_power = std::pow(10.0, -3.0 / 20.0);

    const std::uint32_t centre = index_of(out_mask, MP_SPEAKER_FRONT_CENTER);
    CHECK(matrix.at(centre, 0) == Catch::Approx(half_power));
    CHECK(matrix.at(centre, 1) == Catch::Approx(half_power));

    const std::uint32_t surround = index_of(out_mask, MP_SPEAKER_SIDE_LEFT);
    CHECK(matrix.at(surround, 0) == Catch::Approx(half_power));
    CHECK(matrix.at(surround, 1) == 0.0); // the left surround from the left, only

    // Never the effects channel: an LFE derived from full-range content is a
    // crossover, not a matrix.
    const std::uint32_t lfe = index_of(out_mask, MP_SPEAKER_LOW_FREQUENCY);
    CHECK(matrix.at(lfe, 0) == 0.0);
    CHECK(matrix.at(lfe, 1) == 0.0);
}

TEST_CASE("stereo to mono is the -3 dB sum", "[mix]")
{
    mp::mix::Recipe recipe;
    recipe.normalise = mp::mix::Normalise::none;
    const auto matrix = built(2, 1, recipe, k_stereo, k_mono);
    const double half_power = std::pow(10.0, -3.0 / 20.0);
    CHECK(matrix.at(0, 0) == Catch::Approx(half_power));
    CHECK(matrix.at(0, 1) == Catch::Approx(half_power));
}

TEST_CASE("peak cannot clip and energy keeps the loudness", "[mix]")
{
    // The two normalisations do two different things and the numbers say which.
    mp::mix::Recipe recipe;

    recipe.normalise = mp::mix::Normalise::peak;
    const auto by_peak = built(6, 2, recipe);
    double worst = 0.0;
    for (std::uint32_t i = 0; i < by_peak.inputs; ++i) {
        worst += std::abs(by_peak.at(0, i) * by_peak.scale);
    }
    INFO("peak: rows sum to " << worst << ", scale " << 20.0 * std::log10(by_peak.scale));
    CHECK(worst <= 1.0 + 1e-12); // every input at full scale still fits

    recipe.normalise = mp::mix::Normalise::energy;
    const auto by_energy = built(6, 2, recipe);
    double power = 0.0;
    for (std::uint32_t i = 0; i < by_energy.inputs; ++i) {
        const double c = by_energy.at(0, i) * by_energy.scale;
        power += c * c;
    }
    INFO("energy: unit power, scale " << 20.0 * std::log10(by_energy.scale));
    CHECK(power == Catch::Approx(1.0).margin(1e-12));
    // 3 dB rather than 7.7: the difference between the two, and the reason
    // there are two.
    CHECK(20.0 * std::log10(by_energy.scale) == Catch::Approx(-3.01).margin(0.05));
    CHECK(20.0 * std::log10(by_peak.scale) == Catch::Approx(-7.66).margin(0.05));
}

TEST_CASE("one scale for the whole matrix, so the image does not move", "[mix]")
{
    // Normalising each row on its own would be tidier arithmetic and would put
    // the left and right channels at different levels whenever the layout is
    // not symmetric. This is that property, asserted.
    const auto matrix = built(6, 2);
    for (std::uint32_t i = 0; i < matrix.inputs; ++i) {
        const std::uint32_t mirror = i == 0 ? 1 : (i == 1 ? 0 : (i == 4 ? 5 : (i == 5 ? 4 : i)));
        INFO("input " << i << " against " << mirror);
        CHECK(matrix.at(0, i) == Catch::Approx(matrix.at(1, mirror)));
    }
}

TEST_CASE("a layout nobody agrees on is refused rather than guessed", "[mix]")
{
    mp::mix::Matrix matrix;
    std::string why;
    // Three channels could be L/R/C or L/R/S and the two are not the same
    // recording. Without a mask there is no answer, so there is no answer.
    CHECK_FALSE(mp::mix::build(3, 0, 2, 0, {}, matrix, why));
    CHECK(why.find("conventional layout") != std::string::npos);

    // With one, there is.
    const std::uint32_t three =
        MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_FRONT_CENTER;
    CHECK(mp::mix::build(3, three, 2, 0, {}, matrix, why));
    CHECK(matrix.outputs == 2);
}

TEST_CASE("an explicit matrix is taken exactly as written", "[mix]")
{
    mp::mix::Matrix matrix;
    std::string why;
    REQUIRE(mp::mix::parse_matrix("1,0,0.5;0,1,0.5", 3, 2, matrix, why));
    CHECK(matrix.at(0, 0) == 1.0);
    CHECK(matrix.at(0, 2) == 0.5);
    CHECK(matrix.at(1, 1) == 1.0);
    CHECK(matrix.scale == 1.0); // written means written

    CHECK_FALSE(mp::mix::parse_matrix("1,0;0,1", 3, 2, matrix, why));
    INFO(why);
    CHECK(why.find("3") != std::string::npos);
}

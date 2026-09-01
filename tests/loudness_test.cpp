// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loudness meter, against the numbers the standard publishes for it.
//
// This is the rare component with a *conformance suite*: EBU Tech 3341 names
// signals and says what a compliant meter must read for each, to within a tenth
// of a decibel. So there is nothing to invent here and nothing to calibrate
// against ourselves -- the tests are the standard's own, and a meter that
// passes them is a meter, while one that does not is a number generator.

#include <loudness.hpp>

#include <mediaperch/module.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr double k_pi = 3.14159265358979323846;

/// Runs a signal through in blocks, the way a graph would.
double measure(const std::vector<std::vector<double>>& channels, double rate,
               std::uint32_t mask = 0)
{
    mp::loudness::Meter meter;
    std::string why;
    const bool ok = meter.configure(
        rate, static_cast<std::uint32_t>(channels.size()), mask, why);
    INFO(why);
    REQUIRE(ok);

    std::vector<const double*> planes(channels.size());
    const std::size_t frames = channels[0].size();
    // An awkward block size on purpose: the block boundaries of the meter and
    // of the caller have nothing to do with each other.
    const std::size_t block = 977;
    for (std::size_t at = 0; at < frames; at += block) {
        const auto n = static_cast<std::uint32_t>(std::min(block, frames - at));
        for (std::size_t c = 0; c < channels.size(); ++c) {
            planes[c] = channels[c].data() + at;
        }
        meter.add(planes.data(), n);
    }
    return meter.integrated_lufs();
}

std::vector<double> sine(std::size_t frames, double hz, double rate, double dbfs)
{
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    std::vector<double> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = amplitude * std::sin(2.0 * k_pi * hz * static_cast<double>(n) / rate);
    }
    return out;
}

} // namespace

TEST_CASE("EBU Tech 3341 case 1: a stereo 1 kHz tone reads its own level",
          "[loudness]")
{
    // The calibration point of the whole specification. -23 dBFS in and
    // -23.0 LUFS out, to a tenth of a decibel, and this is what says the
    // K-weighting and the -0.691 offset are both right.
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        const auto frames = static_cast<std::size_t>(rate * 20.0);
        const std::vector<std::vector<double>> stereo{sine(frames, 1000.0, rate, -23.0),
                                                      sine(frames, 1000.0, rate, -23.0)};
        const double measured = measure(stereo, rate);
        INFO(rate << " Hz gave " << measured << " LUFS");
        CHECK(measured == Catch::Approx(-23.0).margin(0.1));
    }
}

TEST_CASE("EBU Tech 3341 case 2: the same tone ten decibels down", "[loudness]")
{
    const double rate = 48000.0;
    const auto frames = static_cast<std::size_t>(rate * 20.0);
    const std::vector<std::vector<double>> stereo{sine(frames, 1000.0, rate, -33.0),
                                                  sine(frames, 1000.0, rate, -33.0)};
    CHECK(measure(stereo, rate) == Catch::Approx(-33.0).margin(0.1));
}

TEST_CASE("the surrounds count for more and the effects channel not at all",
          "[loudness]")
{
    // The standard's weights: 1.0 in front, 1.41 behind, and the LFE left out
    // of the sum. A meter that includes the LFE reads a film mix several
    // decibels loud.
    const double rate = 48000.0;
    const auto frames = static_cast<std::size_t>(rate * 20.0);
    const std::uint32_t mask = MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT |
                               MP_SPEAKER_FRONT_CENTER | MP_SPEAKER_LOW_FREQUENCY |
                               MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;

    const auto quiet = std::vector<double>(frames, 0.0);
    const auto tone = sine(frames, 1000.0, rate, -23.0);

    // Front pair alone, as the reference.
    const double front = measure({tone, tone, quiet, quiet, quiet, quiet}, rate, mask);
    CHECK(front == Catch::Approx(-23.0).margin(0.1));

    // The same tone in the effects channel changes nothing at all.
    const double with_lfe =
        measure({tone, tone, quiet, tone, quiet, quiet}, rate, mask);
    CHECK(with_lfe == Catch::Approx(front).margin(1e-9));

    // And in the surrounds it counts 1.41 times: two more channels at that
    // weight is 10*log10((2 + 2*1.41)/2) = +3.83 LU.
    const double with_surround =
        measure({tone, tone, quiet, quiet, tone, tone}, rate, mask);
    CHECK(with_surround == Catch::Approx(front + 3.83).margin(0.1));
}

TEST_CASE("the gates keep silence from counting", "[loudness]")
{
    // Twenty seconds of tone and twenty of silence should measure what the
    // tone measures, not three decibels less. That is the whole purpose of
    // gating, and the difference between BS.1770-1 and everything after it.
    const double rate = 48000.0;
    const auto half = static_cast<std::size_t>(rate * 20.0);
    auto channel = sine(half, 1000.0, rate, -23.0);
    channel.resize(half * 2, 0.0);
    const double measured = measure({channel, channel}, rate);
    INFO("tone then silence gave " << measured);
    CHECK(measured == Catch::Approx(-23.0).margin(0.15));
}

TEST_CASE("ReplayGain is the distance to the target", "[loudness]")
{
    const double rate = 48000.0;
    const auto frames = static_cast<std::size_t>(rate * 20.0);
    mp::loudness::Meter meter;
    std::string why;
    REQUIRE(meter.configure(rate, 2, 0, why));

    const auto tone = sine(frames, 1000.0, rate, -23.0);
    const double* planes[] = {tone.data(), tone.data()};
    meter.add(planes, static_cast<std::uint32_t>(frames));

    // -23 LUFS wanting to be -18 needs +5 dB.
    CHECK(meter.replay_gain_db() == Catch::Approx(5.0).margin(0.1));
    CHECK(meter.replay_gain_db(-23.0) == Catch::Approx(0.0).margin(0.1));
    // And the peak is the peak of what went in, not of the weighted signal.
    CHECK(meter.sample_peak_db() == Catch::Approx(-23.0).margin(0.01));
}

TEST_CASE("silence is silence rather than a very small number", "[loudness]")
{
    const double rate = 48000.0;
    const std::vector<double> quiet(static_cast<std::size_t>(rate * 5.0), 0.0);
    mp::loudness::Meter meter;
    std::string why;
    REQUIRE(meter.configure(rate, 2, 0, why));
    const double* planes[] = {quiet.data(), quiet.data()};
    meter.add(planes, static_cast<std::uint32_t>(quiet.size()));

    CHECK(meter.integrated_lufs() < -1000.0);
    // And a gain of nothing rather than of infinity, which is what a naive
    // meter hands a player at a track boundary.
    CHECK(meter.replay_gain_db() == 0.0);
}

TEST_CASE("the K-weighting is derived at the rate, not transcribed at one",
          "[loudness]")
{
    // The 48 kHz coefficients are the ones the standard prints, so they are
    // worth pinning: if the derivation drifts, this is where it shows.
    const auto sections = mp::loudness::k_weighting(48000.0);
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].b0 == Catch::Approx(1.53512485958697).margin(1e-10));
    CHECK(sections[0].b1 == Catch::Approx(-2.69169618940638).margin(1e-10));
    CHECK(sections[0].b2 == Catch::Approx(1.19839281085285).margin(1e-10));
    CHECK(sections[0].a1 == Catch::Approx(-1.69065929318241).margin(1e-10));
    CHECK(sections[0].a2 == Catch::Approx(0.73248077421585).margin(1e-10));
    CHECK(sections[1].a1 == Catch::Approx(-1.99004745483398).margin(1e-8));
    CHECK(sections[1].a2 == Catch::Approx(0.99007225036621).margin(1e-8));

    // At another rate they are different numbers describing the same filter:
    // the shelf still turns over at 1682 Hz.
    const auto at_96 = mp::loudness::k_weighting(96000.0);
    mp::biquad::Cascade a;
    mp::biquad::Cascade b;
    a.set_sections(sections, 1);
    b.set_sections(at_96, 1);
    // The response is read at a frequency, so both have to be told their rate;
    // `set_sections` does not know it. Compare the coefficients' own shape
    // instead: the shelf's DC gain is unity at every rate.
    const double dc_48 =
        (sections[0].b0 + sections[0].b1 + sections[0].b2) /
        (1.0 + sections[0].a1 + sections[0].a2);
    const double dc_96 = (at_96[0].b0 + at_96[0].b1 + at_96[0].b2) /
                         (1.0 + at_96[0].a1 + at_96[0].a2);
    CHECK(dc_48 == Catch::Approx(dc_96).margin(1e-9));
    CHECK(dc_48 == Catch::Approx(1.0).margin(1e-9));
}

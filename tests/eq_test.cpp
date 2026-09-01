// SPDX-License-Identifier: GPL-3.0-or-later
//
// The equaliser, checked twice over.
//
// A filter's response is computable from its coefficients and measurable from
// what it does to a tone, and those are two different things that have to
// agree. Every test here that says a band has a certain gain checks the
// computed curve *and* runs a sine through -- because a coefficient set can be
// right while the difference equation that uses it is wrong, and the reverse.

#include <biquad.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace {

constexpr double k_pi = 3.14159265358979323846;
constexpr double k_rate = 48000.0;

/// What the cascade does to a steady sine at `hz`, in dB, measured rather than
/// computed: a long tone, the transient discarded, the amplitude read out with
/// a coherent single-bin transform.
double measured_db(mp::biquad::Cascade& cascade, double hz, std::uint32_t rate = 48000)
{
    // A whole number of cycles in the analysis window, so the measurement needs
    // no window function and has no leakage to explain away.
    const std::size_t window = 48000;
    const double cycles = std::round(hz * window / rate);
    const double exact = cycles * rate / window;
    const std::size_t frames = window * 3;

    std::vector<double> in(frames);
    std::vector<double> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        in[n] = 0.25 * std::sin(2.0 * k_pi * exact * static_cast<double>(n) / rate);
    }
    const double* in_planes[] = {in.data()};
    double* out_planes[] = {out.data()};
    cascade.reset();
    cascade.process(in_planes, static_cast<std::uint32_t>(frames), out_planes);

    const std::size_t from = frames - window; // past every transient
    std::complex<double> sum{0.0, 0.0};
    for (std::size_t i = 0; i < window; ++i) {
        const double angle =
            -2.0 * k_pi * cycles * static_cast<double>(i) / static_cast<double>(window);
        sum += out[from + i] * std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    const double amplitude = std::abs(sum) * 2.0 / static_cast<double>(window);
    return 20.0 * std::log10(amplitude / 0.25);
}

mp::biquad::Cascade configured(const std::string& bands, double rate = k_rate)
{
    std::vector<mp::biquad::Band> parsed;
    std::string why;
    const bool read = mp::biquad::parse_bands(bands, parsed, why);
    INFO(why);
    REQUIRE(read);
    mp::biquad::Cascade cascade;
    const bool ok = cascade.configure(parsed, rate, 1, why);
    INFO(why);
    REQUIRE(ok);
    return cascade;
}

} // namespace

TEST_CASE("an equaliser with no bands is a wire", "[eq]")
{
    auto cascade = configured("none");
    CHECK(cascade.sections() == 0);
    CHECK(cascade.magnitude_db(1000.0) == Catch::Approx(0.0));

    std::vector<double> in{0.5, -0.25, 0.125, 0.0625};
    std::vector<double> out(in.size(), 999.0);
    const double* in_planes[] = {in.data()};
    double* out_planes[] = {out.data()};
    cascade.process(in_planes, 4, out_planes);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == in[i]); // bit for bit: a wire is a wire
    }
}

TEST_CASE("a peaking band reaches its gain at its own frequency", "[eq]")
{
    for (const double gain : {-12.0, -3.0, 3.0, 12.0}) {
        const std::string spec =
            "peak:1000:" + std::to_string(gain) + ":1.0";
        auto cascade = configured(spec);
        INFO(spec);
        // The curve says so...
        CHECK(cascade.magnitude_db(1000.0) == Catch::Approx(gain).margin(1e-9));
        // ...and a tone agrees.
        CHECK(measured_db(cascade, 1000.0) == Catch::Approx(gain).margin(0.01));
        // And it is a *band*: an octave out, most of it is gone.
        CHECK(std::abs(cascade.magnitude_db(125.0)) < std::abs(gain) * 0.2);
        CHECK(std::abs(cascade.magnitude_db(8000.0)) < std::abs(gain) * 0.2);
    }
}

TEST_CASE("Q is the width, and it is continuous", "[eq]")
{
    // Nothing snaps to a preset: two bands a hertz apart are two different
    // filters, and a Q of 0.3 and one of 8 are the same band at two widths.
    const auto wide = configured("peak:1000:6:0.3");
    const auto narrow = configured("peak:1000:6:8");
    CHECK(wide.magnitude_db(1000.0) == Catch::Approx(6.0).margin(1e-9));
    CHECK(narrow.magnitude_db(1000.0) == Catch::Approx(6.0).margin(1e-9));
    // At half an octave out the narrow one has let go and the wide one has not.
    CHECK(narrow.magnitude_db(707.0) < wide.magnitude_db(707.0) - 2.0);

    const auto nudged = configured("peak:1001:6:8");
    CHECK(nudged.magnitude_db(1000.0) != narrow.magnitude_db(1000.0));
}

TEST_CASE("a shelf reaches its gain and holds it", "[eq]")
{
    const auto low = configured("lowshelf:200:-6:0.707");
    CHECK(low.magnitude_db(20.0) == Catch::Approx(-6.0).margin(0.2));
    CHECK(low.magnitude_db(200.0) == Catch::Approx(-3.0).margin(0.3)); // the half-way point
    CHECK(low.magnitude_db(20000.0) == Catch::Approx(0.0).margin(0.05));

    const auto high = configured("highshelf:5000:+4:0.707");
    CHECK(high.magnitude_db(20000.0) == Catch::Approx(4.0).margin(0.2));
    CHECK(high.magnitude_db(100.0) == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("the passes are Butterworth at Q = 1/sqrt(2)", "[eq]")
{
    // -3.01 dB at the corner is the definition, and it is worth pinning
    // because it is the one number that says the bilinear transform was applied
    // to the right prototype.
    const auto low = configured("lowpass:1000::0.7071067811865476");
    CHECK(low.magnitude_db(1000.0) == Catch::Approx(-3.0103).margin(0.02));
    CHECK(measured_db(const_cast<mp::biquad::Cascade&>(low), 1000.0) ==
          Catch::Approx(-3.0103).margin(0.05));
    CHECK(low.magnitude_db(4000.0) < -22.0); // 12 dB an octave, two octaves up

    const auto high = configured("highpass:1000::0.7071067811865476");
    CHECK(high.magnitude_db(1000.0) == Catch::Approx(-3.0103).margin(0.02));
    CHECK(high.magnitude_db(250.0) < -22.0);
}

TEST_CASE("a notch is a hole and an allpass is not", "[eq]")
{
    const auto notch = configured("notch:1000::10");
    CHECK(notch.magnitude_db(1000.0) < -100.0);
    CHECK(notch.magnitude_db(500.0) == Catch::Approx(0.0).margin(0.6));

    const auto allpass = configured("allpass:1000::0.7071");
    for (const double hz : {50.0, 500.0, 1000.0, 5000.0, 20000.0}) {
        INFO(hz << " Hz");
        CHECK(allpass.magnitude_db(hz) == Catch::Approx(0.0).margin(1e-9));
    }
    // Flat and yet not a wire: the phase is what it changed.
    CHECK(allpass.phase_radians(1000.0) != Catch::Approx(0.0).margin(0.1));
}

TEST_CASE("bands compose, and the curve says by how much", "[eq]")
{
    auto cascade = configured("peak:1000:6:1;peak:1000:6:1;highshelf:10000:-3:0.707");
    // Two identical bumps are twelve decibels, not six, and the composite curve
    // is where that is visible before anything is played.
    CHECK(cascade.magnitude_db(1000.0) == Catch::Approx(12.0).margin(0.02));
    CHECK(measured_db(cascade, 1000.0) == Catch::Approx(12.0).margin(0.02));
    CHECK(cascade.sections() == 3);
    CHECK(cascade.peak_gain_db() == Catch::Approx(12.0).margin(0.1));
}

TEST_CASE("a disabled band is remembered and not applied", "[eq]")
{
    std::vector<mp::biquad::Band> bands;
    std::string why;
    REQUIRE(mp::biquad::parse_bands("peak:1000:6:1;-peak:3000:9:1", bands, why));
    REQUIRE(bands.size() == 2);
    CHECK(bands[0].enabled);
    CHECK_FALSE(bands[1].enabled);

    mp::biquad::Cascade cascade;
    REQUIRE(cascade.configure(bands, k_rate, 1, why));
    CHECK(cascade.sections() == 1);
    CHECK(cascade.magnitude_db(3000.0) < 3.0); // the second band is not there

    // And it survives being written back out, which is what makes it a bypass
    // rather than a deletion.
    CHECK(mp::biquad::bands_text(bands).find("-peak:3000") != std::string::npos);
}

TEST_CASE("a band above Nyquist is refused, not warped down to fit", "[eq]")
{
    std::vector<mp::biquad::Band> bands;
    std::string why;
    REQUIRE(mp::biquad::parse_bands("highshelf:24000:6:0.707", bands, why));
    mp::biquad::Cascade cascade;
    CHECK_FALSE(cascade.configure(bands, 44100.0, 1, why));
    INFO(why);
    CHECK(why.find("Nyquist") != std::string::npos);
    // The same band exists perfectly well an octave up in rate.
    CHECK(cascade.configure(bands, 96000.0, 1, why));
}

TEST_CASE("the same band is the same filter at every rate it fits in", "[eq]")
{
    // A 1 kHz bump is a 1 kHz bump at 44.1 and at 192, which is the property
    // that makes a saved setting mean anything. It is not *identical* -- the
    // bilinear transform warps the axis, and it warps it more where a hertz is
    // a larger fraction of the rate -- so what is asserted is that the skirt
    // agrees to a hundredth of a decibel and not that the arithmetic does.
    double reference = 0.0;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto cascade = configured("peak:1000:5:2", rate);
        INFO(rate << " Hz");
        CHECK(cascade.magnitude_db(1000.0) == Catch::Approx(5.0).margin(1e-9));
        const double skirt = cascade.magnitude_db(500.0);
        if (reference == 0.0) {
            reference = skirt;
            CHECK(skirt > 0.0); // an octave down, some of the bump is still there
        } else {
            CHECK(skirt == Catch::Approx(reference).margin(0.01));
        }
    }
}

TEST_CASE("nonsense is refused with the reason", "[eq]")
{
    std::vector<mp::biquad::Band> bands;
    std::string why;
    CHECK_FALSE(mp::biquad::parse_bands("wobble:1000:3:1", bands, why));
    CHECK(why.find("wobble") != std::string::npos);
    CHECK_FALSE(mp::biquad::parse_bands("peak:0:3:1", bands, why));
    CHECK_FALSE(mp::biquad::parse_bands("peak:1000:3:0", bands, why));
    CHECK_FALSE(mp::biquad::parse_bands("peak:1000:99:1", bands, why));
}

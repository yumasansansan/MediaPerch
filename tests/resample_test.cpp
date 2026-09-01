// SPDX-License-Identifier: GPL-3.0-or-later
//
// The resampler, measured against the specification it was designed to.
//
// A resampler is the one component here whose output cannot be checked by
// comparing bytes: everything it produces is new. So it is checked the way a
// filter is checked -- amplitude in the passband, what is left of a tone in the
// stopband, and the noise under a sine that should be clean. The numbers below
// are the design targets from `Design`, not numbers observed once and frozen.
//
// The measurement is coherent: every frequency used here completes a whole
// number of cycles in the window analysed, so a single-bin Goertzel is exact
// and there is no window function anywhere to explain away a result.

#include <resample.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr double k_pi = 3.14159265358979323846;

/// One channel of a sine, at `hz` in a stream running at `rate`.
std::vector<double> sine(std::size_t frames, double hz, std::uint32_t rate,
                         double amplitude = 0.5)
{
    std::vector<double> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = amplitude * std::sin(2.0 * k_pi * hz * static_cast<double>(n) / rate);
    }
    return out;
}

/// The complex amplitude at bin `k` of an `n`-point window starting at `from`.
///
/// Goertzel rather than a DFT because one bin is all this needs, and exact
/// rather than approximate because every frequency used here is an integer
/// number of cycles in the window.
std::complex<double> bin(const std::vector<double>& x, std::size_t from, std::size_t n,
                         double k)
{
    std::complex<double> sum{0.0, 0.0};
    for (std::size_t i = 0; i < n; ++i) {
        const double angle = -2.0 * k_pi * k * static_cast<double>(i) / static_cast<double>(n);
        sum += x[from + i] * std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    return sum * (2.0 / static_cast<double>(n));
}

double rms(const std::vector<double>& x, std::size_t from, std::size_t n)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += x[from + i] * x[from + i];
    }
    return std::sqrt(sum / static_cast<double>(n));
}

/// Everything the tone is not: the residual after the fitted sinusoid is
/// removed, which is distortion plus noise plus every image the filter let
/// through, as dB relative to the tone.
double thd_n_db(const std::vector<double>& x, std::size_t from, std::size_t n, double k)
{
    const std::complex<double> amplitude = bin(x, from, n, k);
    double residual = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double angle = 2.0 * k_pi * k * static_cast<double>(i) / static_cast<double>(n);
        const double fitted = amplitude.real() * std::cos(angle) -
                              amplitude.imag() * std::sin(angle);
        const double error = x[from + i] - fitted;
        residual += error * error;
    }
    const double level = std::abs(amplitude) / std::sqrt(2.0);
    const double noise = std::sqrt(residual / static_cast<double>(n));
    return 20.0 * std::log10(noise / level);
}

/// Runs a whole signal through, in blocks, and drains it. What a graph does.
std::vector<std::vector<double>> run(mp::resample::Resampler& r,
                                     const std::vector<std::vector<double>>& in,
                                     std::uint32_t block)
{
    const std::uint32_t channels = static_cast<std::uint32_t>(in.size());
    const std::size_t frames = in[0].size();
    std::vector<std::vector<double>> out(channels);

    std::vector<std::vector<double>> scratch(channels);
    std::vector<double*> out_planes(channels);
    std::vector<const double*> in_planes(channels);

    const std::uint32_t capacity = r.max_output(block) + 8;
    for (std::uint32_t c = 0; c < channels; ++c) {
        scratch[c].resize(capacity);
        out_planes[c] = scratch[c].data();
    }

    const auto drain = [&](std::uint32_t produced) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[c].insert(out[c].end(), scratch[c].begin(),
                          scratch[c].begin() + static_cast<std::ptrdiff_t>(produced));
        }
    };

    for (std::size_t at = 0; at < frames;) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, frames - at));
        for (std::uint32_t c = 0; c < channels; ++c) {
            in_planes[c] = in[c].data() + at;
        }
        std::uint32_t produced = 0;
        REQUIRE(r.process(in_planes.data(), n, out_planes.data(), capacity, produced));
        drain(produced);
        at += n;
    }

    for (int round = 0; round < 4096; ++round) {
        std::uint32_t produced = 0;
        REQUIRE(r.flush(out_planes.data(), capacity, produced));
        if (produced == 0) {
            break;
        }
        drain(produced);
    }
    return out;
}

mp::resample::Design quality(const char* name)
{
    mp::resample::Design d;
    REQUIRE(mp::resample::design_from_name(name, d));
    return d;
}

} // namespace

TEST_CASE("resampling to the rate it already has is a unit impulse", "[resample]")
{
    // Not a branch: the filter really is designed, and this is what the design
    // comes out as. If it ever stops being one, the branch in `process` is
    // hiding a broken filter rather than skipping a redundant one.
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(44100, 44100, 2, quality("good"), why));
    REQUIRE(r.identity());

    const auto& h = r.prototype();
    REQUIRE(h.size() % 2 == 0);
    const std::size_t centre = h.size() / 2;
    for (std::size_t n = 0; n < h.size(); ++n) {
        INFO("tap " << n);
        REQUIRE(h[n] == Catch::Approx(n == centre ? 1.0 : 0.0).margin(1e-15));
    }
}

TEST_CASE("the ratio is reduced, and an unreducible one is refused by name", "[resample]")
{
    mp::resample::Resampler r;
    std::string why;

    REQUIRE(r.configure(44100, 48000, 2, quality("good"), why));
    CHECK(r.up() == 160);
    CHECK(r.down() == 147);

    REQUIRE(r.configure(96000, 44100, 2, quality("good"), why));
    CHECK(r.up() == 147);
    CHECK(r.down() == 320);

    // 44101/44100 has no common factor, so every phase is a separate filter and
    // there are 44101 of them. Refusing is the honest answer; quietly building
    // a 7-million-tap prototype is not.
    REQUIRE_FALSE(r.configure(44100, 44101, 2, quality("good"), why));
    CHECK(why.find("44101") != std::string::npos);
    CHECK(why.find("coefficients") != std::string::npos);
}

TEST_CASE("the output is exactly as long as the ratio says", "[resample]")
{
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(44100, 48000, 1, quality("good"), why));

    const std::size_t frames = 44100;
    const std::vector<std::vector<double>> in{sine(frames, 1000.0, 44100)};
    const auto out = run(r, in, 1024);

    // ceil(frames * 160 / 147). A frame either way would be a click at a track
    // boundary, so this is an equality and not a tolerance.
    const std::size_t want = (frames * 160 + 146) / 147;
    CHECK(out[0].size() == want);
}

TEST_CASE("a tone comes through at its own amplitude and nothing else does",
          "[resample]")
{
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(44100, 48000, 1, quality("good"), why));

    // One second in, so the analysis window is far from both edges.
    const std::vector<std::vector<double>> in{sine(44100, 1000.0, 44100, 0.5)};
    const auto out = run(r, in, 512);

    // 4800 output frames at 48000 Hz is 0.1 s, which is exactly 100 cycles of
    // 1 kHz: a coherent window, so the measurement needs no window function.
    const std::size_t from = 9600;
    const std::size_t n = 4800;
    REQUIRE(out[0].size() > from + n);

    const double level = std::abs(bin(out[0], from, n, 100.0));
    INFO("amplitude " << level);
    CHECK(level == Catch::Approx(0.5).epsilon(1e-6));

    const double residual = thd_n_db(out[0], from, n, 100.0);
    INFO("THD+N " << residual << " dB");
    // The design asks for 120 dB of stopband; what is left under the tone is
    // images and arithmetic, and it has to stay under that.
    CHECK(residual < -120.0);
}

TEST_CASE("what is above the new Nyquist does not come back as something else",
          "[resample]")
{
    // The failure this catches is the one that matters: a downsampler with no
    // filter turns 30 kHz into a perfectly audible 14.1 kHz tone that was never
    // in the recording.
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(96000, 44100, 1, quality("good"), why));

    const std::vector<std::vector<double>> in{sine(96000, 30000.0, 96000, 0.5)};
    const auto out = run(r, in, 4096);

    const std::size_t from = 8820; // 0.2 s in
    const std::size_t n = 4410;    // 0.1 s at 44100
    REQUIRE(out[0].size() > from + n);

    // 30000 Hz sampled at 44100 folds to 14100 Hz: 1410 cycles in this window.
    const double folded = std::abs(bin(out[0], from, n, 1410.0));
    const double attenuation = 20.0 * std::log10(folded / 0.5);
    INFO("the alias is at " << attenuation << " dB");
    CHECK(attenuation < -120.0);

    // And nothing else got through either.
    const double all = 20.0 * std::log10(rms(out[0], from, n) / (0.5 / std::sqrt(2.0)));
    INFO("everything, at " << all << " dB");
    CHECK(all < -120.0);
}

TEST_CASE("the passband is flat where it was promised to be", "[resample]")
{
    mp::resample::Resampler r;
    std::string why;
    const auto design = quality("good");

    // 0.95 of 22050 Hz is 20947 Hz; every one of these is inside it.
    for (const double hz : {50.0, 400.0, 1000.0, 5000.0, 10000.0, 15000.0, 20000.0}) {
        REQUIRE(r.configure(44100, 48000, 1, design, why));
        const std::vector<std::vector<double>> in{sine(44100, hz, 44100, 0.5)};
        const auto out = run(r, in, 2048);

        const std::size_t from = 9600;
        const std::size_t n = 4800;
        REQUIRE(out[0].size() > from + n);
        const double cycles = hz * static_cast<double>(n) / 48000.0;
        const double level = std::abs(bin(out[0], from, n, cycles));
        const double error_db = 20.0 * std::log10(level / 0.5);
        INFO(hz << " Hz is " << error_db << " dB off");
        // A Kaiser design's passband ripple is about the same size as its
        // stopband leak, so 120 dB down means a ten-thousandth of a decibel.
        CHECK(std::abs(error_db) < 0.001);
    }
}

TEST_CASE("a constant comes out constant, which is unity gain at DC", "[resample]")
{
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(44100, 48000, 1, quality("good"), why));

    const std::vector<std::vector<double>> in{std::vector<double>(44100, 0.25)};
    const auto out = run(r, in, 777);

    // The interior only: the ends are the filter walking into and out of the
    // signal, and a step is not a constant.
    for (std::size_t n = 4000; n < out[0].size() - 4000; ++n) {
        INFO("frame " << n);
        REQUIRE(out[0][n] == Catch::Approx(0.25).margin(1e-9));
    }
}

TEST_CASE("the block size the graph happens to use changes nothing", "[resample]")
{
    // The device decides the period, so the resampler is fed 132 frames on one
    // machine and 4096 on another. Anything that depends on that is a bug that
    // only appears on somebody else's hardware.
    const std::vector<std::vector<double>> in{sine(20000, 997.0, 44100, 0.4)};

    mp::resample::Resampler once;
    std::string why;
    REQUIRE(once.configure(44100, 48000, 1, quality("fast"), why));
    const auto whole = run(once, in, 20000);

    for (const std::uint32_t block : {1u, 7u, 132u, 1000u, 4096u}) {
        mp::resample::Resampler r;
        REQUIRE(r.configure(44100, 48000, 1, quality("fast"), why));
        const auto pieces = run(r, in, block);
        INFO("block " << block);
        REQUIRE(pieces[0].size() == whole[0].size());
        for (std::size_t n = 0; n < whole[0].size(); ++n) {
            REQUIRE(pieces[0][n] == whole[0][n]); // bit for bit, not approximately
        }
    }
}

TEST_CASE("channels do not leak into each other", "[resample]")
{
    mp::resample::Resampler r;
    std::string why;
    REQUIRE(r.configure(48000, 96000, 3, quality("fast"), why));

    std::vector<std::vector<double>> in{sine(9600, 1000.0, 48000, 0.5),
                                        sine(9600, 1000.0, 48000, 0.25),
                                        std::vector<double>(9600, 0.0)};
    const auto out = run(r, in, 333);

    REQUIRE(out[0].size() == out[1].size());
    for (std::size_t n = 0; n < out[0].size(); ++n) {
        INFO("frame " << n);
        // Exactly half, because the filter is linear and the input was.
        REQUIRE(out[1][n] == Catch::Approx(out[0][n] * 0.5).margin(1e-15));
        REQUIRE(out[2][n] == 0.0); // and silence stays silence
    }
}

TEST_CASE("there and back again", "[resample]")
{
    // 44100 -> 48000 -> 44100 is what a file plays through on a device that
    // will not take 44100, twice over. Everything inside the passband should
    // survive the round trip.
    const std::vector<std::vector<double>> in{sine(44100, 997.0, 44100, 0.5)};

    mp::resample::Resampler up;
    mp::resample::Resampler down;
    std::string why;
    REQUIRE(up.configure(44100, 48000, 1, quality("best"), why));
    REQUIRE(down.configure(48000, 44100, 1, quality("best"), why));

    const auto middle = run(up, in, 1024);
    const auto back = run(down, middle, 1024);

    REQUIRE(back[0].size() >= in[0].size());

    // The interior, away from both filters' edges. The delay is compensated at
    // each stage, so frame n should be frame n -- if it is not, the alignment
    // is wrong and every gapless boundary would be too.
    double error = 0.0;
    double signal = 0.0;
    for (std::size_t n = 5000; n < in[0].size() - 5000; ++n) {
        const double d = back[0][n] - in[0][n];
        error += d * d;
        signal += in[0][n] * in[0][n];
    }
    const double snr = 10.0 * std::log10(signal / error);
    INFO("round trip SNR " << snr << " dB");
    CHECK(snr > 130.0);
}

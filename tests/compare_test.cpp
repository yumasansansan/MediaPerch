// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measuring instrument, measured.
//
// `compare` is what decides whether a decoder passes, so a bug in it is worse
// than a bug in a decoder: it either passes something broken or fails something
// correct, and in both cases the thing that is supposed to notice is the thing
// that is wrong. Every case here builds two signals whose relationship is known
// exactly and checks that the instrument reports that relationship.

#include <mediaperch/compare.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace {

constexpr unsigned k_rate = 16000;

/// Broadband, deterministic, and different in every channel: the three things
/// the alignment and channel checks need from a signal.
std::vector<float> noise(std::uint64_t frames, unsigned channels)
{
    std::vector<float> out(frames * channels);
    for (unsigned c = 0; c < channels; ++c) {
        std::uint32_t state = 0x2545F491u + c * 0x9E3779B9u;
        for (std::uint64_t n = 0; n < frames; ++n) {
            state = state * 1664525u + 1013904223u;
            out[n * channels + c] = static_cast<float>(static_cast<std::int32_t>(state)) /
                                    2147483648.0F * 0.4F;
        }
    }
    return out;
}

std::vector<float> tones(std::uint64_t frames, double a_hz, double a_amp, double b_hz,
                         double b_amp)
{
    std::vector<float> out(frames);
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / k_rate;
        out[n] = static_cast<float>(a_amp * std::sin(2.0 * std::numbers::pi * a_hz * t) +
                                    b_amp * std::sin(2.0 * std::numbers::pi * b_hz * t));
    }
    return out;
}

mp::Comparison run(const std::vector<float>& reference, const std::vector<float>& subject,
                   unsigned channels, std::uint32_t band_limit = 0, int max_lag = 256)
{
    return mp::compare(reference.data(), reference.size() / channels, subject.data(),
                       subject.size() / channels, channels, k_rate, band_limit, max_lag);
}

} // namespace

TEST_CASE("a signal compared with itself is reported as identical")
{
    const auto a = noise(8192, 2);
    const mp::Comparison m = run(a, a, 2, 4000);

    CHECK(m.finite);
    CHECK(m.lag == 0);
    CHECK(m.snr_db > 200.0);
    CHECK(m.rms_error == 0.0);
    CHECK(m.channels_in_order);
    CHECK(m.frames_compared == 8192);
    CHECK(m.bands_checked > 0);
    CHECK(m.worst_band_db < 1e-9);
    // And it says so with a clear margin, which is what stops a signal that
    // cannot locate itself from passing quietly.
    CHECK(m.lag_margin_db > 10.0);
    CHECK(m.channel_margin_db > 10.0);
    CHECK(mp::failures(m, mp::Requirements{}).empty());
}

TEST_CASE("a delayed decode is reported as delayed, and measured where it lands")
{
    constexpr std::uint64_t frames = 8192;
    constexpr int delay = 37;
    const auto a = noise(frames, 1);

    // The subject is the reference, `delay` frames late: the shape of every
    // gapless failure this project has measured.
    std::vector<float> late(frames, 0.0F);
    for (std::uint64_t n = delay; n < frames; ++n) {
        late[n] = a[n - delay];
    }

    const mp::Comparison m = run(a, late, 1);
    CHECK(m.lag == delay);
    CHECK(m.lag_margin_db > 10.0);
    // Measured after alignment, so the content still reads as identical -- the
    // delay is one finding and the fidelity is another.
    CHECK(m.snr_db > 200.0);

    const auto why = mp::failures(m, mp::Requirements{});
    REQUIRE(why.size() >= 1);
    CHECK(why.front().find("frames from where the source does") != std::string::npos);

    // And a decode that starts *early* is wrong however lenient the rules are.
    mp::Requirements lenient;
    lenient.exact_length = false;
    lenient.lag_zero = false;
    CHECK(mp::failures(m, lenient).empty());

    const mp::Comparison early = run(late, a, 1);
    CHECK(early.lag == -delay);
    const auto complaints = mp::failures(early, lenient);
    REQUIRE(complaints.size() == 1);
    INFO(complaints.front());
    CHECK(complaints.front().find("before") != std::string::npos);
}

TEST_CASE("two channels swapped is a finding, not a small error")
{
    constexpr std::uint64_t frames = 8192;
    const auto a = noise(frames, 2);
    std::vector<float> swapped(a.size());
    for (std::uint64_t n = 0; n < frames; ++n) {
        swapped[n * 2 + 0] = a[n * 2 + 1];
        swapped[n * 2 + 1] = a[n * 2 + 0];
    }

    const mp::Comparison m = run(a, swapped, 2);
    CHECK_FALSE(m.channels_in_order);
    REQUIRE(m.best_for.size() == 2);
    CHECK(m.best_for[0] == 1);
    CHECK(m.best_for[1] == 0);
    CHECK(m.channel_margin_db < 0.0);

    const auto why = mp::failures(m, mp::Requirements{});
    bool said_so = false;
    for (const auto& line : why) {
        if (line.find("not in the source's order") != std::string::npos) {
            said_so = true;
        }
    }
    CHECK(said_so);
}

TEST_CASE("a band that comes back at the wrong level is found where it is")
{
    constexpr std::uint64_t frames = 16384;
    // Two tones far enough apart that a Hann window cannot smear one into the
    // other, and the second one halved in the subject: a quarter of the energy,
    // which is 6.02 dB.
    const auto a = tones(frames, 1000.0, 0.4, 3000.0, 0.4);
    const auto b = tones(frames, 1000.0, 0.4, 3000.0, 0.2);

    // `max_lag` of zero says "these are known to be aligned, do not search",
    // which is the honest thing to ask for here: see the case below for what
    // two steady tones do to an alignment search.
    const mp::Comparison m = run(a, b, 1, 6000, 0);
    CHECK(m.lag == 0);
    CHECK(m.bands_checked > 0);
    CHECK(m.worst_band_db > 5.0);
    CHECK(m.worst_band_db < 7.0);
    CHECK(m.worst_band_hz > 2700);
    CHECK(m.worst_band_hz < 3300);

    mp::Requirements required;
    required.max_band_db = 2.0;
    const auto why = mp::failures(m, required);
    bool said_so = false;
    for (const auto& line : why) {
        if (line.find("dB from the source's") != std::string::npos) {
            said_so = true;
        }
    }
    CHECK(said_so);

    // Judged on the broadband figure alone the two are 9 dB apart, which a 6 dB
    // floor passes. That is the case for having a band check at all: a level
    // error in one band is a bug, and a broadband number is too blunt to call
    // it one.
    mp::Requirements no_bands;
    no_bands.max_band_db = 0.0;
    no_bands.min_snr_db = 6.0;
    no_bands.min_channel_margin_db = 0.0;
    CHECK(mp::failures(m, no_bands).empty());
}

TEST_CASE("a signal that cannot locate itself is reported as unable to")
{
    // Two steady tones are exactly periodic, so their correlation is too: every
    // multiple of the common period ties for the peak and the one the search
    // lands on is decided by floating-point luck. This is the failure that made
    // the MP3 delay measurement move from a sine to pink noise, and the margin
    // exists so that it announces itself instead of producing a number.
    const auto a = tones(8192, 1000.0, 0.4, 3000.0, 0.4);
    const mp::Comparison m = run(a, a, 1, 0, 256);

    CHECK(m.snr_db > 200.0);       // the signals are identical
    CHECK(m.lag_margin_db < 1.0);  // and the alignment still cannot be trusted

    const auto why = mp::failures(m, mp::Requirements{});
    bool said_so = false;
    for (const auto& line : why) {
        if (line.find("alignment is ambiguous") != std::string::npos) {
            said_so = true;
        }
    }
    CHECK(said_so);
}

TEST_CASE("a decode that is not finite is refused before anything else is read")
{
    const auto a = noise(4096, 1);
    auto broken = a;
    broken[2000] = std::numeric_limits<float>::quiet_NaN();

    const mp::Comparison m = run(a, broken, 1);
    CHECK_FALSE(m.finite);

    const auto why = mp::failures(m, mp::Requirements{});
    REQUIRE(why.size() == 1);
    CHECK(why.front().find("NaN") != std::string::npos);
}

TEST_CASE("a shorter decode is a length failure and not a fidelity one")
{
    constexpr std::uint64_t frames = 8192;
    const auto a = noise(frames, 1);
    const std::vector<float> cut(a.begin(), a.begin() + 6000);

    const mp::Comparison m = mp::compare(a.data(), frames, cut.data(), 6000, 1, k_rate, 0, 256);
    CHECK(m.frames_reference == frames);
    CHECK(m.frames_subject == 6000);
    CHECK(m.lag == 0);
    CHECK(m.snr_db > 200.0); // what is there is right

    const auto why = mp::failures(m, mp::Requirements{});
    REQUIRE(why.size() == 1);
    CHECK(why.front().find("length 6000") != std::string::npos);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading an impulse response, and the four things done to one before it can be
// convolved with anything.
//
// The test writes its own WAV files rather than carrying any: a forty-four byte
// header is short enough to write out, and a reader tested against a file
// somebody committed is a reader tested against one file.

#include <impulse.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// A float32 WAV, written by hand.
std::string write_wav(const std::string& name, const std::vector<std::vector<double>>& channels,
                      std::uint32_t rate)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    const auto channel_count = static_cast<std::uint32_t>(channels.size());
    const auto frames = static_cast<std::uint32_t>(channels.front().size());
    const std::uint32_t data_bytes = frames * channel_count * 4;

    std::vector<std::uint8_t> file;
    const auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto put16 = [&](std::uint16_t v) {
        file.push_back(static_cast<std::uint8_t>(v & 0xFF));
        file.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };
    const auto tag = [&](const char* four) {
        for (int i = 0; i < 4; ++i) {
            file.push_back(static_cast<std::uint8_t>(four[i]));
        }
    };

    tag("RIFF");
    put32(36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(3); // IEEE float
    put16(static_cast<std::uint16_t>(channel_count));
    put32(rate);
    put32(rate * channel_count * 4);
    put16(static_cast<std::uint16_t>(channel_count * 4));
    put16(32);
    tag("data");
    put32(data_bytes);
    for (std::uint32_t n = 0; n < frames; ++n) {
        for (std::uint32_t c = 0; c < channel_count; ++c) {
            const auto value = static_cast<float>(channels[c][n]);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            put32(bits);
        }
    }

    std::FILE* out = std::fopen(path.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    std::fwrite(file.data(), 1, file.size(), out);
    std::fclose(out);
    return path.string();
}

std::vector<double> unit_impulse(std::size_t n, std::size_t at = 0)
{
    std::vector<double> out(n, 0.0);
    out[at] = 1.0;
    return out;
}

} // namespace

TEST_CASE("an impulse response comes back as it was written", "[impulse]")
{
    const auto path = write_wav("mp-impulse-mono.wav", {unit_impulse(64, 3)}, 48000);

    mp::impulse::Response response;
    std::string why;
    INFO(why);
    REQUIRE(mp::impulse::load(path, response, why));
    CHECK(response.sample_rate == 48000);
    REQUIRE(response.channels.size() == 1);
    REQUIRE(response.frames() == 64);
    CHECK(response.channels[0][3] == Catch::Approx(1.0));
    CHECK(response.channels[0][0] == 0.0);

    const auto gains = mp::impulse::measure(response);
    CHECK(gains.dc == Catch::Approx(1.0));
    CHECK(gains.peak == Catch::Approx(1.0));
    CHECK(gains.energy == Catch::Approx(1.0));

    std::filesystem::remove(path);
}

TEST_CASE("a stereo response keeps its two channels apart", "[impulse]")
{
    std::vector<double> left = unit_impulse(32, 1);
    std::vector<double> right = unit_impulse(32, 7);
    right[7] = 0.5;
    const auto path = write_wav("mp-impulse-stereo.wav", {left, right}, 44100);

    mp::impulse::Response response;
    std::string why;
    REQUIRE(mp::impulse::load(path, response, why));
    REQUIRE(response.channels.size() == 2);
    CHECK(response.sample_rate == 44100);
    CHECK(response.channels[0][1] == Catch::Approx(1.0));
    CHECK(response.channels[1][7] == Catch::Approx(0.5));
    CHECK(response.channels[1][1] == 0.0);

    std::filesystem::remove(path);
}

TEST_CASE("one response is broadcast and the wrong number is refused", "[impulse]")
{
    mp::impulse::Response mono;
    mono.sample_rate = 48000;
    mono.channels = {unit_impulse(16)};
    std::string why;
    REQUIRE(mp::impulse::fit_channels(mono, 6, why));
    CHECK(mono.channels.size() == 6);
    CHECK(mono.channels[5][0] == Catch::Approx(1.0));

    mp::impulse::Response three;
    three.sample_rate = 48000;
    three.channels = {unit_impulse(16), unit_impulse(16), unit_impulse(16)};
    // A four- or three-channel file against stereo is somebody's true-stereo
    // matrix, which is a different convolution and is not this one.
    CHECK_FALSE(mp::impulse::fit_channels(three, 2, why));
    INFO(why);
    CHECK(why.find("one to one") != std::string::npos);

    CHECK(mp::impulse::fit_channels(three, 3, why));
}

TEST_CASE("truncation fades rather than cuts", "[impulse]")
{
    mp::impulse::Response response;
    response.sample_rate = 48000;
    response.channels = {std::vector<double>(4000, 1.0)};

    mp::impulse::truncate(response, 1000);
    REQUIRE(response.frames() == 1000);
    // The body is untouched...
    CHECK(response.channels[0][0] == Catch::Approx(1.0));
    CHECK(response.channels[0][800] == Catch::Approx(1.0));
    // ...and the end goes to zero instead of stopping at one, which is the
    // difference between a fade and a click.
    CHECK(response.channels[0][999] < 0.01);
    CHECK(response.channels[0][960] < 0.9);
    CHECK(response.channels[0][960] > 0.0);

    // A response already shorter than the limit is left alone.
    mp::impulse::truncate(response, 100000);
    CHECK(response.frames() == 1000);
}

TEST_CASE("a resampled response is the same filter at the new rate", "[impulse]")
{
    // **The property that matters is the gain, and it is the one that is easy
    // to lose.** A resampler preserves the signal, so an impulse spreads over
    // more samples and its taps sum to more of them -- a wire resampled from
    // 44.1 to 88.2 kHz has taps adding to two. A filter's gain is the sum of
    // its taps at any rate, so it has to come back to one.
    mp::impulse::Response response;
    response.sample_rate = 44100;
    response.channels = {unit_impulse(2048, 1024)};
    const double before = mp::impulse::measure(response).dc;
    CHECK(before == Catch::Approx(1.0));

    std::string why;
    INFO(why);
    REQUIRE(mp::impulse::resample_to(response, 96000, why));
    CHECK(response.sample_rate == 96000);
    // Longer, by about the ratio.
    CHECK(response.frames() > 2048 * 2);

    const double after = mp::impulse::measure(response).dc;
    INFO("DC gain " << before << " became " << after);
    CHECK(after == Catch::Approx(1.0).margin(0.01));

    // And back the other way, from a rate that divides.
    mp::impulse::Response down;
    down.sample_rate = 96000;
    down.channels = {unit_impulse(4096, 2048)};
    REQUIRE(mp::impulse::resample_to(down, 48000, why));
    CHECK(down.sample_rate == 48000);
    CHECK(mp::impulse::measure(down).dc == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("a file that is not audio is refused with the reason", "[impulse]")
{
    mp::impulse::Response response;
    std::string why;
    CHECK_FALSE(mp::impulse::load("no-such-impulse-anywhere.wav", response, why));
    CHECK(why.find("could not read") != std::string::npos);
}

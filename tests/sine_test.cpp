// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/sine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

mp::Format wire_s16()
{
    return mp::Format{.sample_rate = 44100,
                      .channels = 2,
                      .channel_mask = 0,
                      .sample_type = mp::SampleType::s16,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = 0};
}

std::int16_t sample_at(const std::vector<std::uint8_t>& buf, std::size_t frame,
                       std::uint32_t channel, std::uint32_t channels)
{
    const std::size_t index = (frame * channels + channel) * 2;
    std::int16_t v = 0;
    std::memcpy(&v, buf.data() + index, sizeof(v));
    return v;
}

} // namespace

TEST_CASE("the tone fills whole frames and never ends", "[sine]")
{
    mp::SineSource source{wire_s16(), 1000.0};
    std::vector<std::uint8_t> buf(1000);

    // 1000 bytes is 250 frames of 4 bytes, so the odd tail is refused rather
    // than half-written.
    CHECK(source.read(buf.data(), buf.size()) == 1000);
    CHECK(source.frames_produced() == 250);

    std::vector<std::uint8_t> odd(999);
    CHECK(source.read(odd.data(), odd.size()) == 996);

    for (int i = 0; i < 100; ++i) {
        CHECK(source.read(buf.data(), buf.size()) == 1000);
    }
}

TEST_CASE("every channel carries the same tone", "[sine]")
{
    mp::SineSource source{wire_s16(), 1000.0};
    std::vector<std::uint8_t> buf(4 * 512);
    REQUIRE(source.read(buf.data(), buf.size()) == buf.size());

    for (std::size_t frame = 0; frame < 512; ++frame) {
        CHECK(sample_at(buf, frame, 0, 2) == sample_at(buf, frame, 1, 2));
    }
}

TEST_CASE("1 kHz at 44.1 kHz closes after 441 frames and repeats exactly", "[sine]")
{
    // 1000 Hz and 44100 Hz share a factor of 100, so the tone closes over
    // 44100/100 = 441 frames. The phase is taken from a reduced frame index
    // rather than accumulated, so this holds after an hour as much as after a
    // millisecond -- which is what makes an hour-long soak test meaningful.
    mp::SineSource source{wire_s16(), 1000.0};

    constexpr std::size_t cycle_frames = 441;
    constexpr std::size_t bytes = cycle_frames * 4;

    std::vector<std::uint8_t> first(bytes);
    REQUIRE(source.read(first.data(), first.size()) == bytes);

    // Skip a long way ahead: 10,000 whole cycles.
    std::vector<std::uint8_t> skip(bytes);
    for (int i = 0; i < 10'000; ++i) {
        REQUIRE(source.read(skip.data(), skip.size()) == bytes);
    }

    std::vector<std::uint8_t> later(bytes);
    REQUIRE(source.read(later.data(), later.size()) == bytes);
    CHECK(later == first);
}

TEST_CASE("amplitude stays inside the container", "[sine]")
{
    mp::SineSource source{wire_s16(), 1000.0, 0.5};
    std::vector<std::uint8_t> buf(4 * 4410);
    REQUIRE(source.read(buf.data(), buf.size()) == buf.size());

    std::int16_t peak = 0;
    for (std::size_t frame = 0; frame < 4410; ++frame) {
        const std::int16_t v = sample_at(buf, frame, 0, 2);
        if (v > peak) {
            peak = v;
        }
    }
    // Half of full scale, within a sample of the true peak.
    CHECK(peak > 16000);
    CHECK(peak <= 16384);
}

TEST_CASE("the tone is generated in the wire format, not converted into it", "[sine]")
{
    // Path A has no float bus: the generator writes the device's own container
    // directly, and the 24-in-32 case has to come out left-justified with a
    // zero low byte, matching promote().
    mp::Format wide = wire_s16();
    wide.sample_type = mp::SampleType::s24_in_32;
    wide.valid_bits = 24;

    mp::SineSource source{wide, 1000.0};
    std::vector<std::uint8_t> buf(8 * 441);
    REQUIRE(source.read(buf.data(), buf.size()) == buf.size());

    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i] == 0);
    }
}

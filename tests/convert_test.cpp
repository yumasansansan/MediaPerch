// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one component allowed to change a sample, checked against what it claims.
//
// Path A is tested by asserting that bytes came out the way they went in. That
// test does not exist here: the whole point of this code is that they do not.
// So each case states the arithmetic it expects and checks that, which is a
// weaker guarantee honestly made rather than a stronger one implied.

#include <mediaperch/convert.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

mp::Format make(mp::SampleType type, std::uint32_t valid_bits = 0)
{
    return mp::Format{.sample_rate = 48000,
                      .channels = 2,
                      .channel_mask = 0,
                      .sample_type = type,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = valid_bits};
}

std::vector<std::uint8_t> from_floats(const std::vector<float>& v)
{
    std::vector<std::uint8_t> out(v.size() * 4);
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

std::int32_t as_s32(const std::uint8_t* p)
{
    std::int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::int16_t as_s16(const std::uint8_t* p)
{
    std::int16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

mp::ConvertConfig exactly()
{
    mp::ConvertConfig c;
    c.dither = false; // so a value can be asserted rather than a range
    return c;
}

} // namespace

TEST_CASE("a conversion this does not do is refused rather than approximated",
          "[convert]")
{
    const auto f32 = make(mp::SampleType::f32);

    SECTION("a different rate")
    {
        mp::Format other = make(mp::SampleType::s32);
        other.sample_rate = 96000;
        CHECK_FALSE(mp::Converter{f32, other}.possible());
    }
    SECTION("a different channel count")
    {
        mp::Format other = make(mp::SampleType::s32);
        other.channels = 6;
        CHECK_FALSE(mp::Converter{f32, other}.possible());
    }
    SECTION("a format that is not one")
    {
        CHECK_FALSE(mp::Converter{f32, make(mp::SampleType::none)}.possible());
    }
}

TEST_CASE("full scale lands on full scale", "[convert]")
{
    // The convention every decoder and every encoder in this tree uses: an
    // integer sample divided by 2^(bits-1). So -1.0 is exactly the most
    // negative value, and +1.0 is one step past the most positive and clamps.
    const std::vector<float> input{-1.0F, 1.0F, 0.0F, -0.5F};
    const auto bytes = from_floats(input);

    SECTION("to 32-bit")
    {
        std::vector<std::uint8_t> out(input.size() * 4);
        mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s32), exactly()};
        REQUIRE(conv.possible());
        conv.run(bytes.data(), out.data(), input.size() / 2);

        CHECK(as_s32(out.data() + 0) == -2147483647 - 1);
        CHECK(as_s32(out.data() + 4) == 2147483647); // clamped, not wrapped
        CHECK(as_s32(out.data() + 8) == 0);
        CHECK(as_s32(out.data() + 12) == -1073741824);
    }

    SECTION("to 16-bit")
    {
        std::vector<std::uint8_t> out(input.size() * 2);
        mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s16), exactly()};
        conv.run(bytes.data(), out.data(), input.size() / 2);

        CHECK(as_s16(out.data() + 0) == -32768);
        CHECK(as_s16(out.data() + 2) == 32767);
        CHECK(as_s16(out.data() + 4) == 0);
        CHECK(as_s16(out.data() + 6) == -16384);
    }
}

TEST_CASE("a float above full scale is clamped, not wrapped", "[convert]")
{
    // Float WAV routinely holds values past ±1.0 -- that is what float WAV is
    // for -- and an integer device cannot. Wrapping would turn a loud passage
    // into the opposite polarity at full scale, which is the worst sound a
    // player can make.
    const std::vector<float> input{2.5F, -2.5F};
    const auto bytes = from_floats(input);
    std::vector<std::uint8_t> out(4);

    mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s16), exactly()};
    conv.run(bytes.data(), out.data(), 1);

    CHECK(as_s16(out.data() + 0) == 32767);
    CHECK(as_s16(out.data() + 2) == -32768);
}

TEST_CASE("24 bits inside four bytes are left-justified, as the container means",
          "[convert]")
{
    const std::vector<float> input{-1.0F, 0.5F};
    const auto bytes = from_floats(input);
    std::vector<std::uint8_t> out(8);

    mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s24_in_32, 24),
                       exactly()};
    REQUIRE(conv.possible());
    conv.run(bytes.data(), out.data(), 1);

    // -1.0 is the most negative 24-bit value, shifted up by eight.
    CHECK(as_s32(out.data() + 0) == -2147483647 - 1);
    // Half scale, and the low byte is zero because there are only 24 bits of
    // signal in the word.
    CHECK(as_s32(out.data() + 4) == 0x40000000);
    CHECK((as_s32(out.data() + 4) & 0xFF) == 0);
}

TEST_CASE("a gain is the only thing a unity conversion does", "[convert]")
{
    const std::vector<float> input{0.25F, -0.25F};
    const auto bytes = from_floats(input);
    std::vector<std::uint8_t> out(8);

    mp::ConvertConfig half = exactly();
    half.gain = 0.5;
    mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::f32), half};
    REQUIRE(conv.possible());
    CHECK(conv.lossy()); // a gain is a change, and saying so is the point
    conv.run(bytes.data(), out.data(), 1);

    float a = 0.0F;
    float b = 0.0F;
    std::memcpy(&a, out.data() + 0, 4);
    std::memcpy(&b, out.data() + 4, 4);
    CHECK(a == 0.125F);
    CHECK(b == -0.125F);
}

TEST_CASE("float to float at unity gain changes nothing and says so", "[convert]")
{
    const std::vector<float> input{0.3F, -0.7F, 1.9F, -2.4F};
    const auto bytes = from_floats(input);
    std::vector<std::uint8_t> out(bytes.size());

    mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::f32), exactly()};
    REQUIRE(conv.possible());
    CHECK_FALSE(conv.lossy());
    conv.run(bytes.data(), out.data(), input.size() / 2);

    // Including the values past full scale: nothing clamps on the way to float.
    CHECK(std::memcmp(bytes.data(), out.data(), bytes.size()) == 0);
}

TEST_CASE("dither is noise at the last bit, and it is reproducible", "[convert]")
{
    // Half an LSB of a 16-bit destination, held exactly, is the value rounding
    // has no answer for: without dither every one of them goes the same way and
    // the error is a copy of the signal. With it they go both ways.
    std::vector<float> input(2048, 0.5F / 32768.0F);
    const auto bytes = from_floats(input);

    std::vector<std::uint8_t> undithered(input.size() * 2);
    mp::Converter plain{make(mp::SampleType::f32), make(mp::SampleType::s16), exactly()};
    plain.run(bytes.data(), undithered.data(), input.size() / 2);

    std::vector<std::uint8_t> dithered(input.size() * 2);
    mp::Converter noisy{make(mp::SampleType::f32), make(mp::SampleType::s16)};
    noisy.run(bytes.data(), dithered.data(), input.size() / 2);

    int ones_plain = 0;
    int ones_noisy = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        ones_plain += as_s16(undithered.data() + i * 2) != 0 ? 1 : 0;
        ones_noisy += as_s16(dithered.data() + i * 2) != 0 ? 1 : 0;
    }
    // Every sample the same way without it; a mixture with it.
    CHECK((ones_plain == 0 || ones_plain == static_cast<int>(input.size())));
    CHECK(ones_noisy > 0);
    CHECK(ones_noisy < static_cast<int>(input.size()));

    // And the same bytes on the next run, because the generator is seeded
    // rather than sampled from a clock: a difference between two runs of the
    // same file has to mean something.
    std::vector<std::uint8_t> again(input.size() * 2);
    mp::Converter twice{make(mp::SampleType::f32), make(mp::SampleType::s16)};
    twice.run(bytes.data(), again.data(), input.size() / 2);
    CHECK(std::memcmp(dithered.data(), again.data(), again.size()) == 0);
}

TEST_CASE("dither is not applied where it would only add noise", "[convert]")
{
    // Widening cannot lose anything, so there is nothing to dither and adding
    // noise would be vandalism.
    const std::vector<std::uint8_t> input{0x34, 0x12, 0x78, 0x56};
    std::vector<std::uint8_t> out(8);

    mp::Converter conv{make(mp::SampleType::s16), make(mp::SampleType::s32)};
    REQUIRE(conv.possible());
    CHECK_FALSE(conv.lossy());
    conv.run(input.data(), out.data(), 1);

    // 0x1234 at 16 bits is 0x12340000 at 32, exactly.
    CHECK(as_s32(out.data() + 0) == 0x12340000);
    CHECK(as_s32(out.data() + 4) == 0x56780000);
}

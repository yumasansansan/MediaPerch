// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one component allowed to change a sample, checked against what it claims.
//
// Path A is tested by asserting that bytes came out the way they went in. That
// test does not exist here: the whole point of this code is that they do not.
// So each case states the arithmetic it expects and checks that, which is a
// weaker guarantee honestly made rather than a stronger one implied.

#include <mediaperch/convert.hpp>
#include <mediaperch/dither.hpp>
#include <mediaperch/shaper_tables.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>
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
    c.dither = mp::DitherKind::none; // so a value can be asserted, not a range
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

// --------------------------------------------------------------------------
// Dither and noise shaping
//
// The properties here are the defining ones rather than the audible ones: an
// ear is not available to a test, and "sounds better" is not a thing to assert.
// What can be asserted is what each construction is *for*.

TEST_CASE("reading into a double is exact for every type", "[convert][dither]")
{
    // The claim the whole conversion rests on. binary64 has a 53-bit
    // significand, every integer container here is at most 32 bits, and the
    // normalising divisor is a power of two -- so it moves the exponent and
    // leaves the significand alone. A round trip through f64 has to come back
    // byte for byte, or nothing below this line means anything.
    for (const auto type : {mp::SampleType::u8, mp::SampleType::s16,
                            mp::SampleType::s24_packed, mp::SampleType::s32}) {
        const std::size_t width = mp::container_bytes(type);
        std::vector<std::uint8_t> original(width * 2 * 64);
        std::uint32_t state = 12345;
        for (auto& byte : original) {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<std::uint8_t>(state >> 24);
        }

        std::vector<std::uint8_t> wide(8 * 2 * 64);
        std::vector<std::uint8_t> back(original.size());
        mp::Converter up{make(type), make(mp::SampleType::f64), exactly()};
        mp::Converter down{make(mp::SampleType::f64), make(type), exactly()};
        REQUIRE(up.possible());
        REQUIRE(down.possible());
        CHECK_FALSE(up.lossy());

        up.run(original.data(), wide.data(), 64);
        down.run(wide.data(), back.data(), 64);
        INFO("container bytes " << width);
        CHECK(std::memcmp(original.data(), back.data(), original.size()) == 0);
    }
}

TEST_CASE("a gain at the same width still quantises, and still gets dither",
          "[convert][dither]")
{
    // 16 bits to 16 bits is not a widening once there is a gain: half the
    // samples land off the grid. Treating it as one skipped the dither
    // entirely, which is the correlated error the whole file exists to avoid.
    std::vector<std::uint8_t> input(2 * 2 * 512);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const auto v = static_cast<std::int16_t>(1000 + (i % 7));
        std::memcpy(input.data() + i, &v, 2);
    }

    mp::ConvertConfig half;
    half.gain = 0.5;
    mp::Converter conv{make(mp::SampleType::s16), make(mp::SampleType::s16), half};
    REQUIRE(conv.possible());
    CHECK(conv.lossy());
    CHECK(conv.quantising());

    std::vector<std::uint8_t> a(input.size());
    std::vector<std::uint8_t> b(input.size());
    conv.run(input.data(), a.data(), 512);

    mp::ConvertConfig plain = half;
    plain.dither = mp::DitherKind::none;
    mp::Converter without{make(mp::SampleType::s16), make(mp::SampleType::s16), plain};
    without.run(input.data(), b.data(), 512);

    // Dither is doing something, which it was not before.
    CHECK(std::memcmp(a.data(), b.data(), a.size()) != 0);
}

TEST_CASE("every dither kind is reproducible, and none of them is another one",
          "[convert][dither]")
{
    std::vector<float> input(1024, 0.5F / 32768.0F);
    const auto bytes = from_floats(input);

    auto render = [&](mp::DitherKind kind) {
        mp::ConvertConfig c;
        c.dither = kind;
        mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s16), c};
        std::vector<std::uint8_t> out(input.size() * 2);
        conv.run(bytes.data(), out.data(), input.size() / 2);
        return out;
    };

    const auto kinds = {mp::DitherKind::none, mp::DitherKind::rectangular,
                        mp::DitherKind::triangular, mp::DitherKind::highpass_triangular,
                        mp::DitherKind::gaussian};
    std::vector<std::vector<std::uint8_t>> rendered;
    for (const auto kind : kinds) {
        INFO(mp::dither_kind_name(kind));
        const auto once = render(kind);
        CHECK(once == render(kind)); // seeded, so the same twice
        rendered.push_back(once);
    }
    for (std::size_t i = 1; i < rendered.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            CHECK(rendered[i] != rendered[j]);
        }
    }
}

TEST_CASE("highpass dither is tilted where plain triangular dither is not",
          "[convert][dither]")
{
    // The difference between the two is the whole reason to have both. Plain
    // TPDF draws independent values, so successive samples are uncorrelated;
    // the highpass form differences one draw with the last, so successive
    // samples are *anti*-correlated -- which is what "tilted away from the
    // midband" means when it is written as a number rather than a picture.
    auto lag_one = [](mp::DitherKind kind) {
        mp::Dither noise{kind, mp::NoiseShaping{}, 48000, 0x1f2e3d4cu};
        std::vector<double> d(20000);
        for (auto& v : d) {
            v = noise.next();
        }
        double product = 0.0;
        double energy = 0.0;
        for (std::size_t i = 1; i < d.size(); ++i) {
            product += d[i] * d[i - 1];
            energy += d[i] * d[i];
        }
        return product / energy;
    };

    CHECK(std::abs(lag_one(mp::DitherKind::triangular)) < 0.05);
    CHECK(std::abs(lag_one(mp::DitherKind::gaussian)) < 0.05);
    // -0.5 exactly, in the limit: the sequence is r[n] - r[n-1].
    CHECK(lag_one(mp::DitherKind::highpass_triangular) < -0.4);
}

TEST_CASE("noise shaping takes the error out of the band it was in",
          "[convert][dither]")
{
    // A constant input sitting between two codes. Without shaping every sample
    // rounds the same way, so the error is a DC offset and its running sum
    // grows without bound -- which is exactly the audible failure, a quiet
    // passage acquiring a level shift. With a shaper the noise transfer
    // function is (1 - z^-1)^N, whose gain at DC is zero, so the same sum
    // stays bounded however long the block is.
    constexpr std::size_t frames = 4000;
    const float value = 0.3F / 32768.0F;
    std::vector<float> input(frames * 2, value);
    const auto bytes = from_floats(input);

    auto drift = [&](std::uint32_t order) {
        mp::ConvertConfig c;
        c.dither = mp::DitherKind::none; // so the shaper is the only thing acting
        c.shaping.kind = order == 0 ? mp::NoiseShaping::Kind::none
                                    : mp::NoiseShaping::Kind::binomial;
        c.shaping.strength = order;
        mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s16), c};
        std::vector<std::uint8_t> out(input.size() * 2);
        conv.run(bytes.data(), out.data(), frames);

        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            sum += static_cast<double>(as_s16(out.data() + i * 2 * 2)) - 0.3;
        }
        return std::abs(sum);
    };

    CHECK(drift(0) > 1000.0);  // every sample the same way: 0.3 x 4000
    CHECK(drift(1) < 5.0);     // the first-order shaper cancels it
    CHECK(drift(2) < 5.0);
    CHECK(drift(3) < 5.0);
}

TEST_CASE("order zero is off, and the order is what was asked for",
          "[convert][dither]")
{
    mp::ConvertConfig c;
    c.dither = mp::DitherKind::none;
    CHECK(mp::Converter{make(mp::SampleType::f32), make(mp::SampleType::s16), c}
              .shaping_taps() == 0);

    c.shaping.kind = mp::NoiseShaping::Kind::binomial;
    c.shaping.strength = 3;
    CHECK(mp::Converter{make(mp::SampleType::f32), make(mp::SampleType::s16), c}
              .shaping_taps() == 3);

    // Past what the filter holds, clamped rather than read off the end.
    c.shaping.strength = 99;
    CHECK(mp::Converter{make(mp::SampleType::f32), make(mp::SampleType::s16), c}
              .shaping_taps() == mp::NoiseShaping::k_max_order);
}

TEST_CASE("the dither names a user types round-trip", "[convert][dither]")
{
    for (const auto kind : {mp::DitherKind::none, mp::DitherKind::rectangular,
                            mp::DitherKind::triangular,
                            mp::DitherKind::highpass_triangular,
                            mp::DitherKind::gaussian}) {
        mp::DitherKind back{};
        INFO(mp::dither_kind_name(kind));
        REQUIRE(mp::dither_kind_from_name(mp::dither_kind_name(kind), back));
        CHECK(back == kind);
    }

    // The spellings somebody who has used other tools will reach for.
    mp::DitherKind out{};
    CHECK(mp::dither_kind_from_name("tpdf", out));
    CHECK(out == mp::DitherKind::triangular);
    CHECK(mp::dither_kind_from_name("rpdf", out));
    CHECK(out == mp::DitherKind::rectangular);

    CHECK_FALSE(mp::dither_kind_from_name("shibata", out));
    CHECK_FALSE(mp::dither_kind_from_name("", out));
}

// --------------------------------------------------------------------------
// The measured curves

TEST_CASE("the transcribed curves are the ones in the source", "[convert][shaper]")
{
    // Spot values read out of SSRC's shapercoefs.h by eye. A generator that
    // silently wrote most of a table is the failure this guards -- it happened
    // once already, when two entries carrying a trailing comment did not match
    // the pattern and 65 of 67 curves were emitted without a word.
    const mp::ShaperCurve* curve = mp::find_shaper(44100, 0);
    REQUIRE(curve != nullptr);
    CHECK(curve->length == 12);
    CHECK(curve->coefficients[0] == -0.59543782472610473633);
    CHECK(curve->coefficients[11] == -0.031739629805088043213);

    CHECK(mp::shaper_curves().size() == 67);

    // Every rate a decoder in this tree can produce at CD and DVD rates.
    CHECK(mp::find_shaper(48000, 0) != nullptr);
    CHECK(mp::find_shaper(96000, 1) != nullptr);

    // "No shaper" is a curve with no taps, and asking for it gets nothing
    // rather than an empty filter to run.
    CHECK(mp::find_shaper(44100, 99) == nullptr);

    // A rate with no curves at all. Quietly using one fitted for another rate
    // would put the noise an octave from where it belongs.
    CHECK(mp::find_shaper(176400, 0) == nullptr);
    CHECK(mp::highest_intensity(44100) == 16);
    CHECK(mp::highest_intensity(176400) == 0);
}

TEST_CASE("a measured curve shapes, and an absent one does nothing",
          "[convert][shaper]")
{
    constexpr std::size_t frames = 4000;
    std::vector<float> input(frames * 2, 0.3F / 32768.0F);
    const auto bytes = from_floats(input);

    auto drift = [&](mp::NoiseShaping shaping) {
        mp::ConvertConfig c;
        c.dither = mp::DitherKind::none;
        c.shaping = shaping;
        mp::Converter conv{make(mp::SampleType::f32), make(mp::SampleType::s16), c};
        std::vector<std::uint8_t> out(input.size() * 2);
        conv.run(bytes.data(), out.data(), frames);
        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            sum += static_cast<double>(as_s16(out.data() + i * 2 * 2)) - 0.3;
        }
        return std::abs(sum);
    };

    mp::NoiseShaping off;
    mp::NoiseShaping shibata{mp::NoiseShaping::Kind::shibata, 3};

    // The ATH curves are not DC-nulling the way a binomial shaper is -- they
    // are fitted to a hearing threshold, not to a corner at zero -- so the
    // assertion is that they move the error a long way, not that they cancel
    // it exactly.
    CHECK(drift(off) > 1000.0);
    CHECK(drift(shibata) < drift(off) / 10.0);

    // 48 kHz has curves, this rate has none, and the difference is visible:
    // nothing happens rather than something wrong happening.
    mp::ConvertConfig c;
    c.dither = mp::DitherKind::none;
    c.shaping = shibata;
    mp::Format odd = make(mp::SampleType::s16);
    odd.sample_rate = 176400;
    mp::Format odd_in = make(mp::SampleType::f32);
    odd_in.sample_rate = 176400;
    CHECK(mp::Converter{odd_in, odd, c}.shaping_taps() == 0);
    CHECK(mp::Converter{make(mp::SampleType::f32), make(mp::SampleType::s16), c}
              .shaping_taps() > 0);
}

TEST_CASE("the shaper names a user types are read the way they look",
          "[convert][shaper]")
{
    mp::NoiseShaping out;

    REQUIRE(mp::noise_shaping_from_name("0", out));
    CHECK(out.kind == mp::NoiseShaping::Kind::none);

    REQUIRE(mp::noise_shaping_from_name("3", out));
    CHECK(out.kind == mp::NoiseShaping::Kind::binomial);
    CHECK(out.strength == 3);

    REQUIRE(mp::noise_shaping_from_name("shibata", out));
    CHECK(out.kind == mp::NoiseShaping::Kind::shibata);
    CHECK(out.strength == 0);

    REQUIRE(mp::noise_shaping_from_name("shibata:6", out));
    CHECK(out.strength == 6);

    CHECK_FALSE(mp::noise_shaping_from_name("10", out));      // past the order
    CHECK_FALSE(mp::noise_shaping_from_name("shibata6", out)); // missing colon
    CHECK_FALSE(mp::noise_shaping_from_name("lipshitz", out)); // not transcribed
    CHECK_FALSE(mp::noise_shaping_from_name("", out));

    // And the report says what actually happened, not what was asked for.
    CHECK(mp::noise_shaping_describe({mp::NoiseShaping::Kind::shibata, 0}, 44100)
              .starts_with("shibata: ATH"));
    CHECK(mp::noise_shaping_describe({mp::NoiseShaping::Kind::shibata, 0}, 176400)
              .find("no curve") != std::string::npos);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/negotiation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

mp::Format cd_audio()
{
    return mp::Format{.sample_rate = 44100,
                      .channels = 2,
                      .channel_mask = 0,
                      .sample_type = mp::SampleType::s16,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = 0};
}

bool contains(const std::vector<mp::Candidate>& list, const mp::Format& f)
{
    return std::ranges::any_of(list, [&](const mp::Candidate& c) { return c.format == f; });
}

} // namespace

TEST_CASE("the first candidate is always the source, unchanged", "[negotiation]")
{
    const auto source = cd_audio();
    const auto candidates = mp::build_candidates(source);

    REQUIRE_FALSE(candidates.empty());
    CHECK(candidates.front().format == source);
    CHECK(candidates.front().fidelity == mp::Fidelity::exact);
    CHECK_FALSE(candidates.front().channel_mask_added);
}

TEST_CASE("the extensible form is paired with its base, not appended at the end",
          "[negotiation]")
{
    // A driver that simply wants a channel mask must not cause a needless
    // widening: exact+mask has to be tried before the first wider container.
    const auto candidates = mp::build_candidates(cd_audio());
    REQUIRE(candidates.size() >= 3);

    CHECK(candidates[0].fidelity == mp::Fidelity::exact);
    CHECK_FALSE(candidates[0].channel_mask_added);

    CHECK(candidates[1].fidelity == mp::Fidelity::exact);
    CHECK(candidates[1].channel_mask_added);
    CHECK(candidates[1].format.channel_mask == mp::conventional_channel_mask(2));

    CHECK(candidates[2].fidelity == mp::Fidelity::widened);
}

TEST_CASE("widening keeps the valid-bit count, so nothing claims more than it has",
          "[negotiation]")
{
    const auto candidates = mp::build_candidates(cd_audio());

    for (const auto& c : candidates) {
        CHECK(c.format.sample_rate == 44100);
        CHECK(c.format.channels == 2);
        CHECK(mp::effective_valid_bits(c.format) == 16);
        CHECK(mp::is_bit_exact(c.fidelity));
    }

    mp::Format in_32 = cd_audio();
    in_32.sample_type = mp::SampleType::s24_in_32;
    in_32.valid_bits = 16;
    CHECK(contains(candidates, in_32));
}

TEST_CASE("the list stops before any conversion", "[negotiation]")
{
    const auto candidates = mp::build_candidates(cd_audio());

    for (const auto& c : candidates) {
        // No rate change, no channel change, no narrowing, no float.
        CHECK(c.format.sample_rate == 44100);
        CHECK(c.format.channels == 2);
        CHECK(c.format.sample_type != mp::SampleType::f32);
        CHECK(mp::container_bytes(c.format.sample_type) >= 2);
        CHECK(mp::classify(cd_audio(), c.format) == c.fidelity);
    }
}

TEST_CASE("a 32-bit source has nowhere wider to go", "[negotiation]")
{
    auto source = cd_audio();
    source.sample_type = mp::SampleType::s32;

    const auto candidates = mp::build_candidates(source);
    CHECK(candidates.size() == 2); // itself, and itself with a mask
    for (const auto& c : candidates) {
        CHECK(c.fidelity == mp::Fidelity::exact);
    }
}

TEST_CASE("DoP and bitstreams are never widened", "[negotiation]")
{
    // Shifting a DoP frame moves the 0x05/0xFA markers into the sample bits and
    // the DAC stops recognising it as DSD.
    mp::Format dop{.sample_rate = 176400,
                   .channels = 2,
                   .channel_mask = 0,
                   .sample_type = mp::SampleType::s24_in_32,
                   .encoding = mp::Encoding::dop,
                   .valid_bits = 24};

    auto candidates = mp::build_candidates(dop);
    REQUIRE_FALSE(candidates.empty());
    for (const auto& c : candidates) {
        CHECK(c.fidelity == mp::Fidelity::exact);
        CHECK(c.format.sample_type == mp::SampleType::s24_in_32);
    }

    mp::Format bitstream = dop;
    bitstream.encoding = mp::Encoding::iec61937;
    bitstream.sample_type = mp::SampleType::s16;
    bitstream.sample_rate = 48000;
    bitstream.valid_bits = 16;

    candidates = mp::build_candidates(bitstream);
    for (const auto& c : candidates) {
        CHECK(c.fidelity == mp::Fidelity::exact);
        CHECK(c.format.sample_type == mp::SampleType::s16);
    }
}

TEST_CASE("a source with a mask already gets no extra tagged variant", "[negotiation]")
{
    auto source = cd_audio();
    source.channel_mask = mp::conventional_channel_mask(2);

    const auto candidates = mp::build_candidates(source);
    for (const auto& c : candidates) {
        CHECK_FALSE(c.channel_mask_added);
        CHECK(c.format.channel_mask == source.channel_mask);
    }
}

TEST_CASE("an invalid source produces no candidates rather than a guess", "[negotiation]")
{
    mp::Format nonsense{};
    CHECK(mp::build_candidates(nonsense).empty());
}

TEST_CASE("classify names what the sink actually did", "[negotiation]")
{
    const auto source = cd_audio();

    SECTION("identical is exact")
    {
        CHECK(mp::classify(source, source) == mp::Fidelity::exact);
    }

    SECTION("naming a layout the source left open changes no bytes")
    {
        auto tagged = source;
        tagged.channel_mask = mp::conventional_channel_mask(2);
        CHECK(mp::classify(source, tagged) == mp::Fidelity::exact);
    }

    SECTION("a wider container holding the same bits is widened")
    {
        auto wider = source;
        wider.sample_type = mp::SampleType::s32;
        wider.valid_bits = 16;
        CHECK(mp::classify(source, wider) == mp::Fidelity::widened);
    }

    SECTION("a rate change is a conversion")
    {
        auto resampled = source;
        resampled.sample_rate = 48000;
        CHECK(mp::classify(source, resampled) == mp::Fidelity::converted);
    }

    SECTION("a channel change is a conversion")
    {
        auto downmixed = source;
        downmixed.channels = 1;
        downmixed.channel_mask = 0;
        CHECK(mp::classify(source, downmixed) == mp::Fidelity::converted);
    }

    SECTION("reordering a layout the source did name is a conversion")
    {
        auto named = source;
        named.channel_mask = mp::conventional_channel_mask(2);

        auto reordered = named;
        reordered.channel_mask = MP_SPEAKER_BACK_LEFT | MP_SPEAKER_BACK_RIGHT;
        CHECK(mp::classify(named, reordered) == mp::Fidelity::converted);
    }

    SECTION("float is a conversion however lossless it looks")
    {
        auto as_float = source;
        as_float.sample_type = mp::SampleType::f32;
        as_float.valid_bits = 0;
        CHECK(mp::classify(source, as_float) == mp::Fidelity::converted);
    }

    SECTION("a narrower container is a conversion")
    {
        auto source24 = source;
        source24.sample_type = mp::SampleType::s24_in_32;
        source24.valid_bits = 24;

        auto narrowed = source;
        narrowed.sample_type = mp::SampleType::s16;
        narrowed.valid_bits = 16;
        CHECK(mp::classify(source24, narrowed) == mp::Fidelity::converted);
    }
}

TEST_CASE("is_bit_exact covers exactly the two the passthrough graph may serve",
          "[negotiation]")
{
    STATIC_REQUIRE(mp::is_bit_exact(mp::Fidelity::exact));
    STATIC_REQUIRE(mp::is_bit_exact(mp::Fidelity::widened));
    STATIC_REQUIRE_FALSE(mp::is_bit_exact(mp::Fidelity::converted));
}

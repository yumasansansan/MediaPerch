// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/negotiation.hpp"

#include "mediaperch/format.hpp"

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
    // repack: exact+mask has to be tried before any other container. Measured on
    // a real device, which refuses the plain form and takes the extensible one.
    const auto candidates = mp::build_candidates(cd_audio());
    REQUIRE(candidates.size() >= 3);

    CHECK(candidates[0].fidelity == mp::Fidelity::exact);
    CHECK_FALSE(candidates[0].channel_mask_added);

    CHECK(candidates[1].fidelity == mp::Fidelity::exact);
    CHECK(candidates[1].channel_mask_added);
    CHECK(candidates[1].format.channel_mask == mp::conventional_channel_mask(2));

    CHECK(candidates[2].fidelity == mp::Fidelity::repacked);
}

TEST_CASE("repacking keeps the valid-bit count, so nothing claims more than it has",
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

TEST_CASE("a 32-bit source has nowhere else to go", "[negotiation]")
{
    // Two and three byte containers cannot hold 32 valid bits, so there is no
    // repack to offer and the list is the source and its extensible form.
    auto source = cd_audio();
    source.sample_type = mp::SampleType::s32;

    const auto candidates = mp::build_candidates(source);
    CHECK(candidates.size() == 2);
    for (const auto& c : candidates) {
        CHECK(c.fidelity == mp::Fidelity::exact);
    }
}

TEST_CASE("both 24-bit containers are offered, because devices want different ones",
          "[negotiation]")
{
    // "24 bit" names two wire formats: three bytes packed, and 24 valid bits
    // inside four. Measured on this machine, a VB-Audio virtual cable configured
    // for 24-bit wanted the three-byte form and the onboard Realtek codec wanted
    // the four-byte one. Offering only one of them refuses playable audio.
    mp::Format source = cd_audio();
    source.sample_type = mp::SampleType::s24_in_32;
    source.valid_bits = 24;

    const auto candidates = mp::build_candidates(source);

    CHECK(candidates.front().format.sample_type == mp::SampleType::s24_in_32);
    CHECK(candidates.front().fidelity == mp::Fidelity::exact);

    mp::Format packed = source;
    packed.sample_type = mp::SampleType::s24_packed;
    CHECK(contains(candidates, packed));

    // And the two-byte container is not offered: it cannot hold 24 bits.
    for (const auto& c : candidates) {
        CHECK(mp::container_bytes(c.format.sample_type) >= 3);
        CHECK(mp::effective_valid_bits(c.format) == 24);
        CHECK(mp::is_bit_exact(c.fidelity));
    }
}

TEST_CASE("a 16-bit source is offered every container that can hold it", "[negotiation]")
{
    const auto candidates = mp::build_candidates(cd_audio());

    // Two, three and four bytes, each with and without a channel mask.
    CHECK(candidates.size() == 6);

    bool two = false;
    bool three = false;
    bool four = false;
    for (const auto& c : candidates) {
        switch (mp::container_bytes(c.format.sample_type)) {
        case 2: two = true; break;
        case 3: three = true; break;
        case 4: four = true; break;
        default: FAIL("unexpected container");
        }
    }
    CHECK(two);
    CHECK(three);
    CHECK(four);
}

TEST_CASE("the same wire format is never offered twice under two names",
          "[negotiation]")
{
    // s24_in_32 with 16 valid bits and s32 with 16 valid bits are the same
    // WAVEFORMATEXTENSIBLE. Candidates are generated over containers so that the
    // list cannot contain both and waste a device round trip on the duplicate.
    for (const auto type : {mp::SampleType::s16, mp::SampleType::s24_packed,
                            mp::SampleType::s24_in_32, mp::SampleType::s32}) {
        mp::Format source = cd_audio();
        source.sample_type = type;
        source.valid_bits = mp::natural_valid_bits(type);

        const auto candidates = mp::build_candidates(source);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                const bool same_container =
                    mp::container_bytes(candidates[i].format.sample_type) ==
                    mp::container_bytes(candidates[j].format.sample_type);
                const bool same_mask = candidates[i].format.channel_mask ==
                                       candidates[j].format.channel_mask;
                CHECK_FALSE((same_container && same_mask));
            }
        }
    }
}

TEST_CASE("DoP and bitstreams are never repacked", "[negotiation]")
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

    SECTION("a different container holding the same bits is repacked")
    {
        auto wider = source;
        wider.sample_type = mp::SampleType::s32;
        wider.valid_bits = 16;
        CHECK(mp::classify(source, wider) == mp::Fidelity::repacked);
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

    SECTION("losing valid bits is a conversion, whatever the container")
    {
        auto source24 = source;
        source24.sample_type = mp::SampleType::s24_in_32;
        source24.valid_bits = 24;

        auto narrowed = source;
        narrowed.sample_type = mp::SampleType::s16;
        narrowed.valid_bits = 16;
        CHECK(mp::classify(source24, narrowed) == mp::Fidelity::converted);
    }

    SECTION("a smaller container that still holds every valid bit is repacked")
    {
        // The case a real device asked for: 24 valid bits leaving a four-byte
        // container for a three-byte one. Nothing is lost, so this is not a
        // narrowing however it looks from the container sizes.
        auto source24 = source;
        source24.sample_type = mp::SampleType::s24_in_32;
        source24.valid_bits = 24;

        auto packed = source24;
        packed.sample_type = mp::SampleType::s24_packed;
        packed.valid_bits = 24;
        CHECK(mp::classify(source24, packed) == mp::Fidelity::repacked);
    }
}

TEST_CASE("is_bit_exact covers exactly the two the passthrough graph may serve",
          "[negotiation]")
{
    STATIC_REQUIRE(mp::is_bit_exact(mp::Fidelity::exact));
    STATIC_REQUIRE(mp::is_bit_exact(mp::Fidelity::repacked));
    STATIC_REQUIRE_FALSE(mp::is_bit_exact(mp::Fidelity::converted));
}

// --------------------------------------------------------------------------
// Choosing the path by hand
//
// Until there was a policy the answer was always "every bit-exact container the
// device might take". That is the right default and these check it is still
// what `automatic` does -- and that the other two mean what they say, because a
// setting that quietly does the same thing as the default is worse than no
// setting.

TEST_CASE("asking for a memcpy offers the source's container and nothing else",
          "[negotiation][path]")
{
    const auto full = mp::build_candidates(cd_audio(), mp::PathPolicy::bit_exact);
    const auto exact = mp::build_candidates(cd_audio(), mp::PathPolicy::exact_only);

    REQUIRE_FALSE(exact.empty());
    CHECK(exact.size() < full.size());
    for (const auto& candidate : exact) {
        INFO(mp::describe(candidate.format));
        CHECK(candidate.fidelity == mp::Fidelity::exact);
        CHECK(candidate.format.sample_type == cd_audio().sample_type);
    }
    // The extensible form is still offered: naming a layout the source left
    // unspecified moves no bytes, so it is a memcpy too.
    CHECK(exact.size() == 2);
    CHECK(exact.front() == full.front());
}

TEST_CASE("asking for the processed graph still offers the source's own format",
          "[negotiation][path]")
{
    // A gain does not need a different wire format. Forcing one would quantise
    // a 16-bit file into some other container for no reason at all, so the
    // source's own container is offered first here as everywhere else -- what
    // changes is which graph runs, not which format is asked for.
    const auto only_b = mp::build_candidates(cd_audio(), mp::PathPolicy::processed);
    REQUIRE_FALSE(only_b.empty());
    CHECK(contains(only_b, cd_audio()));
    CHECK(only_b.front().format == cd_audio());

    // And Path B is what runs, whatever the format relationship turns out to be.
    CHECK(mp::use_processed(mp::PathPolicy::processed, mp::Fidelity::exact));
    CHECK(mp::use_processed(mp::PathPolicy::processed, mp::Fidelity::converted));
    CHECK_FALSE(mp::use_processed(mp::PathPolicy::bit_exact, mp::Fidelity::exact));
    CHECK_FALSE(mp::use_processed(mp::PathPolicy::automatic, mp::Fidelity::repacked));
    CHECK(mp::use_processed(mp::PathPolicy::automatic, mp::Fidelity::converted));
}

TEST_CASE("the default is bit-exact, which is the option that can refuse",
          "[negotiation][path]")
{
    CHECK(mp::build_candidates(cd_audio()) ==
          mp::build_candidates(cd_audio(), mp::PathPolicy::bit_exact));
    for (const auto& candidate : mp::build_candidates(cd_audio())) {
        CHECK(candidate.fidelity != mp::Fidelity::converted);
    }
}

TEST_CASE("auto tries every bit-exact answer before it converts one",
          "[negotiation][path]")
{
    const auto list = mp::build_candidates(cd_audio(), mp::PathPolicy::automatic);
    const auto exact = mp::build_candidates(cd_audio(), mp::PathPolicy::bit_exact);
    REQUIRE(list.size() > exact.size());

    // The bit-exact list, in its own order, is the front of this one.
    CHECK(std::equal(exact.begin(), exact.end(), list.begin()));
    for (std::size_t i = exact.size(); i < list.size(); ++i) {
        INFO(mp::describe(list[i].format));
        CHECK(list[i].fidelity == mp::Fidelity::converted);
    }
}

TEST_CASE("a float source has no repack and every conversion", "[negotiation][path]")
{
    // The case Path B was written for: every lossy decoder here reports F32,
    // and an endpoint that will not take float leaves the file unplayable.
    mp::Format source = cd_audio();
    source.sample_type = mp::SampleType::f32;

    const auto strict = mp::build_candidates(source, mp::PathPolicy::bit_exact);
    for (const auto& candidate : strict) {
        CHECK(candidate.format.sample_type == mp::SampleType::f32);
    }

    const auto loose = mp::build_candidates(source, mp::PathPolicy::automatic);
    CHECK(loose.size() > strict.size());
    CHECK(std::ranges::any_of(loose, [](const mp::Candidate& c) {
        return c.fidelity == mp::Fidelity::converted &&
               c.format.sample_type == mp::SampleType::s32;
    }));
}

TEST_CASE("a policy allows exactly what its name says", "[negotiation][path]")
{
    CHECK(mp::allows(mp::PathPolicy::exact_only, mp::Fidelity::exact));
    CHECK_FALSE(mp::allows(mp::PathPolicy::exact_only, mp::Fidelity::repacked));
    CHECK_FALSE(mp::allows(mp::PathPolicy::exact_only, mp::Fidelity::converted));

    CHECK(mp::allows(mp::PathPolicy::bit_exact, mp::Fidelity::exact));
    CHECK(mp::allows(mp::PathPolicy::bit_exact, mp::Fidelity::repacked));
    CHECK_FALSE(mp::allows(mp::PathPolicy::bit_exact, mp::Fidelity::converted));

    CHECK(mp::allows(mp::PathPolicy::automatic, mp::Fidelity::exact));
    CHECK(mp::allows(mp::PathPolicy::automatic, mp::Fidelity::repacked));
    CHECK(mp::allows(mp::PathPolicy::automatic, mp::Fidelity::converted));

    // Any format at all: what `processed` decides is the graph, not the wire.
    CHECK(mp::allows(mp::PathPolicy::processed, mp::Fidelity::exact));
    CHECK(mp::allows(mp::PathPolicy::processed, mp::Fidelity::repacked));
    CHECK(mp::allows(mp::PathPolicy::processed, mp::Fidelity::converted));
}

TEST_CASE("the names a user types round-trip", "[negotiation][path]")
{
    for (const auto policy : {mp::PathPolicy::bit_exact, mp::PathPolicy::exact_only,
                              mp::PathPolicy::automatic, mp::PathPolicy::processed}) {
        mp::PathPolicy back{};
        INFO(mp::path_policy_name(policy));
        REQUIRE(mp::path_policy_from_name(mp::path_policy_name(policy), back));
        CHECK(back == policy);
    }

    mp::PathPolicy ignored{};
    CHECK_FALSE(mp::path_policy_from_name("", ignored));
    CHECK_FALSE(mp::path_policy_from_name("bit-exact", ignored));
    CHECK_FALSE(mp::path_policy_from_name("convert", ignored));
    CHECK_FALSE(mp::path_policy_from_name("AUTO", ignored));
}

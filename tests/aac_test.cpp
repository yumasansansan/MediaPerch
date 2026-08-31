// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AAC-LC parser's refusals, and the one property its tables must have.
//
// What this decoder gets right on real files is established elsewhere, by
// parsing every packet of six of them and checking that each frame ends exactly
// on its packet boundary -- 585 packets, zero slack. That check is worth more
// than any unit test here could be, because a single misread bit desynchronises
// a frame and it then cannot land on the boundary by luck.
//
// What is here is the other half: the configurations that must be turned away,
// and the fact that the codebooks build at all.

#include <aac.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

/// A real AudioSpecificConfig: AAC-LC, 48 kHz, stereo.
std::array<std::uint8_t, 2> lc_stereo_48k()
{
    // 00010 0011 0010 000 -> AOT 2, freq index 3, channel config 2, GA config 0
    return {0x11, 0x90};
}

} // namespace

TEST_CASE("a real AudioSpecificConfig parses into the values it encodes")
{
    const auto asc = lc_stereo_48k();
    mp::aac::Config cfg;
    REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
    CHECK(cfg.object_type == 2);
    CHECK(cfg.sample_rate == 48000);
    CHECK(cfg.rate_index == 3);
    CHECK(cfg.channel_config == 2);
    CHECK_FALSE(cfg.frame_960);
}

TEST_CASE("the standard's thirteen sample rates are the ones this decoder knows")
{
    CHECK(mp::aac::rate_for_index(0) == 96000);
    CHECK(mp::aac::rate_for_index(3) == 48000);
    CHECK(mp::aac::rate_for_index(4) == 44100);
    CHECK(mp::aac::rate_for_index(11) == 8000);
    CHECK(mp::aac::rate_for_index(12) == 7350);
    CHECK(mp::aac::rate_for_index(13) == 0);
    CHECK(mp::aac::rate_for_index(255) == 0);
}

TEST_CASE("a configuration this decoder does not implement is refused, not guessed at")
{
    mp::aac::Config cfg;

    SECTION("too short to be a config")
    {
        const auto asc = lc_stereo_48k();
        CHECK_FALSE(mp::aac::parse_asc(asc.data(), 1, cfg));
        CHECK_FALSE(mp::aac::parse_asc(nullptr, 2, cfg));
    }

    SECTION("HE-AAC and HE-AACv2, which are object types 5 and 29")
    {
        // Only the core would decode, at half the sample rate, and sounding
        // like it. Refusing sends the file to decode_ffmpeg instead.
        for (std::uint8_t aot : {std::uint8_t{5}, std::uint8_t{29}}) {
            std::array<std::uint8_t, 2> asc{};
            asc[0] = static_cast<std::uint8_t>(aot << 3);
            asc[1] = 0x90;
            CHECK_FALSE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
        }
    }

    SECTION("object types that are not AAC-LC at all")
    {
        for (std::uint8_t aot : {std::uint8_t{1}, std::uint8_t{3}, std::uint8_t{4},
                                 std::uint8_t{23}}) {
            std::array<std::uint8_t, 2> asc{};
            asc[0] = static_cast<std::uint8_t>(aot << 3);
            asc[1] = 0x90;
            CHECK_FALSE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
        }
    }

    SECTION("a channel configuration with no layout")
    {
        // The fields do not fall on byte boundaries -- 5 bits of object type,
        // 4 of rate index, 4 of channel configuration, then the GA config --
        // so the bytes are built rather than poked at.
        for (unsigned channels : {8u, 9u, 10u, 15u}) {
            const unsigned aot = 2;
            const unsigned rate = 3;
            std::array<std::uint8_t, 2> asc{};
            asc[0] = static_cast<std::uint8_t>((aot << 3) | (rate >> 1));
            asc[1] = static_cast<std::uint8_t>(((rate & 1u) << 7) | (channels << 3));
            INFO("channel configuration " << channels);
            CHECK_FALSE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
        }
    }

    SECTION("7.1 is configuration 12, and is accepted")
    {
        std::array<std::uint8_t, 2> asc{};
        asc[0] = static_cast<std::uint8_t>((2u << 3) | (3u >> 1));
        asc[1] = static_cast<std::uint8_t>(((3u & 1u) << 7) | (12u << 3));
        REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
        CHECK(cfg.channel_config == 12);
    }
}

TEST_CASE("the codebooks build, which is the only thing a table can promise")
{
    // init() builds the twelve Huffman tries and fails if any codeword is a
    // prefix of another. The generator has already checked the Kraft equality;
    // this checks that the codewords themselves form a usable code, which is a
    // different claim and the one that decoding depends on.
    mp::aac::Config cfg;
    const auto asc = lc_stereo_48k();
    REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));

    mp::aac::Decoder decoder;
    INFO(decoder.error());
    CHECK(decoder.init(cfg));
}

TEST_CASE("a frame of junk is refused rather than acted on")
{
    mp::aac::Config cfg;
    const auto asc = lc_stereo_48k();
    REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));

    mp::aac::Decoder decoder;
    REQUIRE(decoder.init(cfg));

    SECTION("an empty packet")
    {
        const std::uint8_t nothing = 0;
        CHECK_FALSE(decoder.decode_frame(&nothing, 0));
    }

    SECTION("bytes that are not a frame")
    {
        std::array<std::uint8_t, 512> junk{};
        for (std::size_t i = 0; i < junk.size(); ++i) {
            junk[i] = static_cast<std::uint8_t>(i * 61u + 7u);
        }
        // The only requirement is that it returns. A hostile packet is allowed
        // to look decodable, and this one may well parse -- what it must not do
        // is spin, which is what the first version of read_section_data did.
        decoder.decode_frame(junk.data(), junk.size());
        SUCCEED();
    }
}

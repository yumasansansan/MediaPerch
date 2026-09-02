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
#include <vector>

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

TEST_CASE("channel configuration 0 takes its layout from the program config element")
{
    // FFmpeg writes configuration 0 for 7.1(wide), putting the layout in a
    // program_config_element inside the AudioSpecificConfig rather than in the
    // configuration field. This is that config, taken from a real file: one
    // front SCE, two front CPEs, one back CPE and an LFE.
    const std::array<std::uint8_t, 26> asc{0x11, 0x80, 0x04, 0xCC, 0x05, 0x00, 0x01, 0x08, 0xC8,
                                           0x00, 0x0C, 0x4C, 0x61, 0x76, 0x63, 0x36, 0x33, 0x2E,
                                           0x31, 0x2E, 0x31, 0x30, 0x31, 0x56, 0xE5, 0x00};
    mp::aac::Config cfg;
    REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
    CHECK(cfg.channel_config == 0);
    CHECK(cfg.sample_rate == 48000);

    REQUIRE(cfg.pce.count == 8);
    CHECK(cfg.pce.mask == 0xFFu); // FL FR FC LFE BL BR FLC FRC

    // The elements arrive as C, then the two front pairs, then the back pair,
    // then the LFE -- and front pairs are listed from the centre outwards, so
    // the first pair is front-left-of-centre and the second is front left.
    // Reading those the other way round decodes every channel perfectly and
    // still puts two of them in the wrong speakers.
    const std::array<std::uint8_t, 8> want{3, 4, 0, 7, 5, 6, 1, 2};
    for (unsigned slot = 0; slot < 8; ++slot) {
        INFO("WAVE slot " << slot);
        CHECK(cfg.pce.from[slot] == want[slot]);
    }
}

TEST_CASE("configuration 0 without a program config element is refused")
{
    // Nothing in the file says what the channels are, so there is nothing to
    // guess from. This is the same two bytes as the stereo config with the
    // configuration field zeroed.
    const std::array<std::uint8_t, 2> asc{0x11, 0x80};
    mp::aac::Config cfg;
    CHECK_FALSE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));
}

TEST_CASE("init restarts the noise generator, so a file decodes the same way twice")
{
    // Noise-substituted bands are filled from a generator carried in the
    // decoder. If init() leaves it where the last decode stopped, the second
    // decode of a file differs from the first. That is not hypothetical: the
    // host used to read a frame to prove a decoder worked and then seek back,
    // so *every* file whose length was known was decoded from a generator two
    // frames out of step. The bands were the right size and the right energy
    // and held the wrong noise.
    //
    // The frame below is the first of a 48 kHz mono stream at 64 kbit/s, which
    // substitutes noise in eight of its bands. Without PNS this test would pass
    // whatever init() did.
    const std::array<std::uint8_t, 2> asc{0x11, 0x88}; // AAC-LC, 48 kHz, mono
    static const std::uint8_t frame[] = {
        0xDC, 0x00, 0x4C, 0x61, 0x76, 0x63, 0x36, 0x33, 0x2E, 0x31, 0x2E, 0x31, 0x30, 0x31,
        0x00, 0x02, 0x8C, 0xA9, 0x52, 0xE1, 0xBA, 0x18, 0x16, 0x86, 0x8B, 0xA2, 0x81, 0x69,
        0x17, 0x7E, 0xB5, 0xCF, 0x9A, 0xAC, 0xF8, 0xDE, 0x71, 0x7C, 0xF3, 0xED, 0x4D, 0x21,
        0xF8, 0xBA, 0xA9, 0x15, 0x22, 0xB2, 0x48, 0x2B, 0x5A, 0xE5, 0x9F, 0x09, 0x68, 0x44,
        0x31, 0x09, 0x3D, 0x10, 0xA3, 0x1D, 0xC9, 0xBF, 0xBA, 0x30, 0x89, 0xBC, 0x7E, 0xE9,
        0xC2, 0x18, 0x20, 0xF4, 0x32, 0x26, 0x38, 0x0A, 0x89, 0x57, 0x71, 0x63, 0xB6, 0xF0,
        0x05, 0xF2, 0x13, 0x03, 0xCD, 0xA9, 0x67, 0x71, 0xC0, 0xA6, 0x18, 0x33, 0x6D, 0x9D,
        0x80, 0x64, 0x44, 0x91, 0x79, 0xF2, 0x1B, 0x3D, 0xDF, 0x0F, 0xBB, 0xB7, 0x86, 0xDF,
        0xB0, 0xD0, 0x55, 0x8C, 0xEB, 0x75, 0x39, 0x54, 0x6B, 0xB8, 0xB7, 0x74, 0x16, 0xC6,
        0x5B, 0x56, 0x7C, 0x64, 0xD5, 0xB7, 0x57, 0x7F, 0xBB, 0x8C, 0x8D, 0x2D, 0x06, 0xEF,
        0xC8, 0xA9, 0x28, 0xD4, 0x19, 0xB7, 0x3F, 0x91, 0x8B, 0xE1, 0x9D, 0xBA, 0x76, 0xF9,
        0x04, 0xE0, 0x5C, 0xA0, 0x02, 0x01, 0xAF, 0xD3, 0x83, 0x26, 0x54, 0xD8, 0xF3, 0xDE,
        0xF1, 0x6F, 0xF1, 0x61, 0xF1, 0xF1, 0x04, 0xD0, 0xF9, 0x0B, 0x7E, 0x3E, 0x4D, 0xFD,
        0x3B, 0xF8, 0xEB, 0xDD, 0xAA, 0xEC, 0x14, 0x86, 0x2A, 0x76, 0x39, 0x38, 0xFC, 0x3F,
        0x5B, 0xB7, 0x5E, 0x31, 0x66, 0xA4, 0x2B, 0x03, 0x88, 0x8B, 0xDE, 0x3D, 0xD0, 0xF5,
        0xC7, 0xC2, 0x7C, 0x1E, 0xF6, 0x5B, 0xC4, 0xA9, 0x86, 0x55, 0x42, 0x37, 0x09, 0xE2,
        0x31, 0xE3, 0x18, 0x18, 0x03, 0xC0, 0x2E, 0xAD, 0x2D, 0x87, 0xD8, 0xB4, 0xD8, 0x56,
        0x36, 0x3B, 0x8D, 0x65, 0x87, 0x81, 0x77, 0x1C, 0x6A, 0x5C, 0x3F, 0x3E, 0x36, 0xB1,
        0x57, 0xA7, 0x9A, 0xAE, 0x41, 0x0C, 0x23, 0xC0};

    mp::aac::Config cfg;
    REQUIRE(mp::aac::parse_asc(asc.data(), asc.size(), cfg));

    mp::aac::Decoder decoder;
    REQUIRE(decoder.init(cfg));
    REQUIRE(decoder.decode_frame(frame, sizeof(frame)));
    REQUIRE(decoder.channels() == 1);
    std::vector<float> first(decoder.pcm(0), decoder.pcm(0) + mp::aac::k_frame_len);

    REQUIRE(decoder.init(cfg));
    REQUIRE(decoder.decode_frame(frame, sizeof(frame)));

    bool same = true;
    for (unsigned i = 0; i < mp::aac::k_frame_len; ++i) {
        if (first[i] != decoder.pcm(0)[i]) {
            INFO("sample " << i << ": " << first[i] << " then " << decoder.pcm(0)[i]);
            same = false;
            break;
        }
    }
    CHECK(same);

    // And the frame is not silence, so the comparison above meant something.
    float peak = 0.0F;
    for (float v : first) {
        peak = v > peak ? v : (-v > peak ? -v : peak);
    }
    CHECK(peak > 0.0F);
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

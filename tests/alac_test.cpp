// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ALAC decoder's refusals, which are most of what it does.
//
// The bit-exactness of this decoder is established elsewhere and against real
// files -- docs/formats.md has the matrix, and it is checked by hashing a decode
// against the WAV that was encoded. What is checked here is the other half: that
// a cookie or a packet describing something impossible is turned away rather
// than acted on. Those are the paths a fuzzer explores and a listener never
// does, so they are the ones a regression can hide in.

#include <alac.hpp>
#include <mp4.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace {

/// A real cookie, taken from a 44.1 kHz 24-bit stereo file written by refalac.
std::array<std::uint8_t, 24> good_cookie()
{
    return {0x00, 0x00, 0x10, 0x00,  // frameLength 4096
            0x00,                    // compatibleVersion
            0x18,                    // bitDepth 24
            0x28, 0x0a, 0x0e,        // pb, mb, kb
            0x02,                    // channels
            0x00, 0xff,              // maxRun
            0x00, 0x00, 0x00, 0x00,  // maxFrameBytes
            0x00, 0x00, 0x00, 0x00,  // avgBitRate
            0x00, 0x00, 0xac, 0x44}; // sampleRate 44100
}

} // namespace

TEST_CASE("a real ALAC magic cookie parses into the values it encodes")
{
    const auto cookie = good_cookie();
    mp::alac::Config cfg;
    REQUIRE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
    CHECK(cfg.frame_length == 4096);
    CHECK(cfg.bit_depth == 24);
    CHECK(cfg.channels == 2);
    CHECK(cfg.sample_rate == 44100);
    CHECK(cfg.max_run == 255);
}

TEST_CASE("a cookie describing something impossible is refused")
{
    mp::alac::Config cfg;

    SECTION("too short to be a cookie at all")
    {
        const auto cookie = good_cookie();
        CHECK_FALSE(mp::alac::parse_config(cookie.data(), 23, cfg));
        CHECK_FALSE(mp::alac::parse_config(nullptr, 24, cfg));
    }

    SECTION("a bit depth ALAC does not define")
    {
        // 16, 20, 24 and 32 are the format's; everything else here would reach
        // an output path that does not exist.
        for (std::uint8_t depth : {std::uint8_t{0}, std::uint8_t{8}, std::uint8_t{17},
                                   std::uint8_t{31}, std::uint8_t{64}}) {
            auto cookie = good_cookie();
            cookie[5] = depth;
            CHECK_FALSE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
        }
    }

    SECTION("more channels than ALAC has a layout for")
    {
        auto cookie = good_cookie();
        cookie[9] = 9;
        CHECK_FALSE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
        cookie[9] = 0;
        CHECK_FALSE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
    }

    SECTION("a frame length that is really an allocation request")
    {
        auto cookie = good_cookie();
        cookie[0] = 0x7f;
        cookie[1] = 0xff;
        cookie[2] = 0xff;
        cookie[3] = 0xff;
        CHECK_FALSE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
    }

    SECTION("a compatible-version this decoder has never seen")
    {
        auto cookie = good_cookie();
        cookie[4] = 1;
        CHECK_FALSE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));
    }
}

TEST_CASE("every ALAC channel layout is a permutation, not a rearrangement that loses one")
{
    // The bug this guards against is the one Media Foundation ships: a table
    // that moves channels around and drops or duplicates one is indetectable by
    // ear on most material and produces exactly the wrong speaker feed.
    for (unsigned channels = 1; channels <= 8; ++channels) {
        const mp::alac::ChannelLayout& l = mp::alac::layout_for(channels);

        std::array<int, 8> seen{};
        for (unsigned slot = 0; slot < channels; ++slot) {
            REQUIRE(l.from[slot] < channels);
            seen[l.from[slot]] += 1;
        }
        for (unsigned c = 0; c < channels; ++c) {
            INFO("channels = " << channels << ", source channel " << c);
            CHECK(seen[c] == 1);
        }

        // Mono and stereo report no mask, matching every other decoder here.
        if (channels <= 2) {
            CHECK(l.mask == 0u);
        } else {
            unsigned bits = 0;
            for (unsigned b = 0; b < 32; ++b) {
                bits += (l.mask >> b) & 1u;
            }
            INFO("channels = " << channels);
            CHECK(bits == channels);
        }
    }
}

TEST_CASE("a decoder handed junk produces nothing and says so")
{
    const auto cookie = good_cookie();
    mp::alac::Config cfg;
    REQUIRE(mp::alac::parse_config(cookie.data(), cookie.size(), cfg));

    mp::alac::Decoder decoder;
    REQUIRE(decoder.init(cfg));

    std::vector<std::int32_t> out(static_cast<std::size_t>(cfg.frame_length) * cfg.channels, 0);

    SECTION("an empty packet")
    {
        const std::uint8_t nothing = 0;
        CHECK(decoder.decode(&nothing, 0, out.data()) == 0);
    }

    SECTION("bytes that are not an ALAC frame")
    {
        std::vector<std::uint8_t> junk(512);
        for (std::size_t i = 0; i < junk.size(); ++i) {
            junk[i] = static_cast<std::uint8_t>(i * 37u + 11u);
        }
        // The only requirement is that it returns rather than misbehaves; a
        // hostile packet is allowed to look decodable.
        const std::uint32_t frames = decoder.decode(junk.data(), junk.size(), out.data());
        CHECK(frames <= cfg.frame_length);
    }

    SECTION("a decoder that was never initialised")
    {
        mp::alac::Decoder fresh;
        const std::uint8_t byte = 0;
        CHECK(fresh.decode(&byte, 1, out.data()) == 0);
    }
}

TEST_CASE("a moov box that is not an ALAC track is declined without a diagnosis of luck")
{
    mp::mp4::AudioTrack track;
    const char* why = "";

    CHECK_FALSE(mp::mp4::parse_moov(nullptr, 0, track, &why));

    std::vector<std::uint8_t> junk(256, 0xAB);
    CHECK_FALSE(mp::mp4::parse_moov(junk.data(), junk.size(), track, &why));

    // A box whose declared size reaches past the buffer it sits in. The parser
    // has to notice; the buffer is not going to tell it.
    const std::uint8_t liar[] = {0x7f, 0xff, 0xff, 0xff, 't', 'r', 'a', 'k', 0x00, 0x00};
    CHECK_FALSE(mp::mp4::parse_moov(liar, sizeof(liar), track, &why));
}

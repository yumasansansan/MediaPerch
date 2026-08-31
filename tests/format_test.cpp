// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/format.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

TEST_CASE("container sizes are what the wire expects", "[format]")
{
    CHECK(mp::container_bytes(mp::SampleType::s16) == 2);
    CHECK(mp::container_bytes(mp::SampleType::s24_packed) == 3);
    CHECK(mp::container_bytes(mp::SampleType::s24_in_32) == 4);
    CHECK(mp::container_bytes(mp::SampleType::s32) == 4);
    CHECK(mp::container_bytes(mp::SampleType::f32) == 4);
    CHECK(mp::container_bytes(mp::SampleType::none) == 0);
}

TEST_CASE("frame size and rate", "[format]")
{
    const auto f = cd_audio();
    CHECK(mp::frame_bytes(f) == 4);
    CHECK(mp::bytes_per_second(f) == 176400);

    mp::Format hires = f;
    hires.sample_rate = 192000;
    hires.sample_type = mp::SampleType::s24_in_32;
    CHECK(mp::frame_bytes(hires) == 8);
    CHECK(mp::bytes_per_second(hires) == 1536000);
}

TEST_CASE("valid bits default to the container and are reported when they differ",
          "[format]")
{
    auto f = cd_audio();
    CHECK(mp::effective_valid_bits(f) == 16);

    // 16-bit content sitting in a 24-in-32 container: the field that keeps a
    // widened candidate honest.
    f.sample_type = mp::SampleType::s24_in_32;
    f.valid_bits = 16;
    CHECK(mp::effective_valid_bits(f) == 16);
    CHECK(mp::describe(f).find("16 valid") != std::string::npos);
}

TEST_CASE("is_valid rejects what no sink could be asked for", "[format]")
{
    CHECK(mp::is_valid(cd_audio()));

    auto no_rate = cd_audio();
    no_rate.sample_rate = 0;
    CHECK_FALSE(mp::is_valid(no_rate));

    auto no_channels = cd_audio();
    no_channels.channels = 0;
    CHECK_FALSE(mp::is_valid(no_channels));

    auto no_type = cd_audio();
    no_type.sample_type = mp::SampleType::none;
    CHECK_FALSE(mp::is_valid(no_type));

    auto too_many_bits = cd_audio();
    too_many_bits.valid_bits = 24; // will not fit in two bytes
    CHECK_FALSE(mp::is_valid(too_many_bits));
}

TEST_CASE("a channel mask has to name exactly as many positions as there are channels",
          "[format]")
{
    auto stereo = cd_audio();
    stereo.channel_mask = mp::conventional_channel_mask(2);
    CHECK(mp::is_valid(stereo));

    // A 5.1 mask on a stereo stream leaves the sink to guess, which is what the
    // extensible form exists to stop.
    stereo.channel_mask = mp::conventional_channel_mask(6);
    CHECK_FALSE(mp::is_valid(stereo));
}

TEST_CASE("float is never a wire format for DoP or a bitstream", "[format]")
{
    auto f = cd_audio();
    f.sample_type = mp::SampleType::f32;
    f.encoding = mp::Encoding::dop;
    CHECK_FALSE(mp::is_valid(f));

    f.encoding = mp::Encoding::iec61937;
    CHECK_FALSE(mp::is_valid(f));

    f.encoding = mp::Encoding::pcm;
    CHECK(mp::is_valid(f));
}

TEST_CASE("conventional masks exist only where there is one obvious answer", "[format]")
{
    CHECK(mp::conventional_channel_mask(1) == MP_SPEAKER_FRONT_CENTER);
    CHECK(mp::conventional_channel_mask(2) ==
          (MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT));
    CHECK(mp::conventional_channel_mask(6) != 0);
    CHECK(mp::conventional_channel_mask(8) != 0);

    // Guessing a layout for these is worse than leaving the sink its default.
    CHECK(mp::conventional_channel_mask(3) == 0);
    CHECK(mp::conventional_channel_mask(5) == 0);
    CHECK(mp::conventional_channel_mask(7) == 0);
    CHECK(mp::conventional_channel_mask(16) == 0);
}

TEST_CASE("describe says enough to diagnose a refused negotiation", "[format]")
{
    auto f = cd_audio();
    f.channel_mask = mp::conventional_channel_mask(2);
    const std::string text = mp::describe(f);

    CHECK(text.find("44100 Hz") != std::string::npos);
    CHECK(text.find("2 ch") != std::string::npos);
    CHECK(text.find("S16") != std::string::npos);
    CHECK(text.find("mask 0x3") != std::string::npos);
}

TEST_CASE("the two Path B-only sample types describe themselves correctly")
{
    // U8 and F64 exist so that a decoder can report what a file holds rather
    // than narrowing it in silence. Neither can ever be a wire format -- no
    // endpoint takes eight-bit or sixty-four-bit samples -- so the thing to
    // check is that they are described honestly and that negotiation leaves
    // them alone.
    CHECK(mp::container_bytes(mp::SampleType::u8) == 1);
    CHECK(mp::natural_valid_bits(mp::SampleType::u8) == 8);

    CHECK(mp::container_bytes(mp::SampleType::f64) == 8);
    CHECK(mp::natural_valid_bits(mp::SampleType::f64) == 64);

    // canonical_for maps integer containers, and 1 and 8 are not among them:
    // an unsigned byte and an IEEE double are not points on the signed,
    // left-justified line the repacker works in.
    CHECK(mp::canonical_for(1, 8) == mp::SampleType::none);
    CHECK(mp::canonical_for(8, 64) == mp::SampleType::none);
}

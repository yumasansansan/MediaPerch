// SPDX-License-Identifier: GPL-3.0-or-later
//
// The policy and the file it reads. No device, because neither of them has one:
// what a measurement *costs* is a real device at real speed, and what a
// measurement *means* is arithmetic.

#include "mediaperch/buffering.hpp"
#include "mediaperch/profile.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

mp::StreamShape uhd()
{
    return mp::StreamShape{MP_CODEC_HEVC, 3840, 2160, 24000, 1001};
}

mp::StreamShape hd()
{
    return mp::StreamShape{MP_CODEC_HEVC, 1920, 1080, 24000, 1001};
}

mp::Measurement measured(const mp::StreamShape& shape, double ring_ms)
{
    mp::Measurement one;
    one.shape = shape;
    one.ring_ms = ring_ms;
    one.runs = 2;
    return one;
}

// This machine's numbers, so the arithmetic below is the arithmetic that runs:
// 144 frames at 48 kHz is the 3.0 ms period §9.8.2 measured.
constexpr std::uint32_t k_period = 144;
constexpr std::uint32_t k_rate = 48000;
constexpr std::uint32_t k_default = 128;

} // namespace

TEST_CASE("a profile with nothing to say leaves the default alone", "[buffering]")
{
    const mp::Profile empty;
    CHECK(mp::ring_for(uhd(), empty, k_period, k_rate, k_default) == k_default);
    CHECK(mp::answer_for(uhd(), empty) == nullptr);
}

TEST_CASE("a measurement moves the ring in whichever direction it measured",
          "[buffering]")
{
    mp::Profile profile;
    // 96 ms is 32 periods at 3 ms, below the default of 128.
    profile.measured.push_back(measured(uhd(), 96.0));
    CHECK(mp::ring_for(uhd(), profile, k_period, k_rate, k_default) == 32);

    // **And above it, which is the case a default cannot answer.** 128 periods
    // is generous against the 4K measurement in §9.8.2 and says nothing about
    // material heavier than that; 16K60 beside f64 PCM would make 683 ms a
    // small ring. A measurement is the only thing that knows, and clamping it
    // to the default would leave that class glitching with its answer already
    // written down. 4000 ms is 1333.3 periods, so 1334.
    mp::Profile heavy;
    heavy.measured.push_back(measured(uhd(), 4000.0));
    CHECK(mp::ring_for(uhd(), heavy, k_period, k_rate, k_default) == 1334);
}

TEST_CASE("a heavier class answers for a lighter one, and never the reverse",
          "[buffering]")
{
    mp::Profile profile;
    profile.measured.push_back(measured(uhd(), 96.0));

    // 1080p is cheaper than the 4K that was measured, so the 4K answer is safe
    // for it: wasteful, which is the direction this is willing to be wrong in.
    const mp::Measurement* for_hd = mp::answer_for(hd(), profile);
    REQUIRE(for_hd != nullptr);
    CHECK(for_hd->shape == uhd());

    // 8K is not. Nothing measured is at least as expensive, so there is no
    // answer, and no answer is the default rather than an extrapolation.
    const mp::StreamShape uhd8k{MP_CODEC_HEVC, 7680, 4320, 60, 1};
    CHECK(mp::answer_for(uhd8k, profile) == nullptr);
    CHECK(mp::ring_for(uhd8k, profile, k_period, k_rate, k_default) == k_default);
}

TEST_CASE("the cheapest class above is the one that answers", "[buffering]")
{
    mp::Profile profile;
    profile.measured.push_back(measured(uhd(), 96.0));
    profile.measured.push_back(measured(mp::StreamShape{MP_CODEC_HEVC, 7680, 4320, 60, 1},
                                        480.0));
    // Both are above 1080p. The cheaper of the two is the closer fit, and the
    // 8K answer would be four times the ring for no reason.
    const mp::Measurement* answer = mp::answer_for(hd(), profile);
    REQUIRE(answer != nullptr);
    CHECK(answer->shape == uhd());
}

TEST_CASE("an exact class beats a heavier one that also fits", "[buffering]")
{
    mp::Profile profile;
    profile.measured.push_back(measured(uhd(), 480.0));
    profile.measured.push_back(measured(hd(), 48.0));
    const mp::Measurement* answer = mp::answer_for(hd(), profile);
    REQUIRE(answer != nullptr);
    CHECK(answer->ring_ms == 48.0);
}

TEST_CASE("a stream that states no frame rate stands in for nothing", "[buffering]")
{
    // A container that timestamps every frame instead of stating a rate leaves
    // fps at 0/0, so there is no place on the axis to compare it. It can still
    // match itself exactly, and that is all.
    mp::StreamShape untimed = uhd();
    untimed.fps_num = 0;
    untimed.fps_den = 0;

    mp::Profile profile;
    profile.measured.push_back(measured(untimed, 96.0));
    CHECK(mp::answer_for(hd(), profile) == nullptr);
    CHECK(mp::answer_for(untimed, profile) != nullptr);
    CHECK(mp::samples_per_second(untimed) == 0.0);
}

TEST_CASE("the ring is rounded up, because half a period short was not measured",
          "[buffering]")
{
    mp::Profile profile;
    // 100 ms is 33.3 periods at 3 ms. 33, not 33.3 and not 33 rounded down.
    profile.measured.push_back(measured(uhd(), 100.0));
    CHECK(mp::ring_for(uhd(), profile, k_period, k_rate, k_default) == 34);
}

TEST_CASE("a device that has said nothing about itself gets the default",
          "[buffering]")
{
    mp::Profile profile;
    profile.measured.push_back(measured(uhd(), 96.0));
    CHECK(mp::ring_for(uhd(), profile, 0, k_rate, k_default) == k_default);
    CHECK(mp::ring_for(uhd(), profile, k_period, 0, k_default) == k_default);
}

TEST_CASE("the dimensions a user picks have names, and only the real ones",
          "[buffering]")
{
    mp::Dimension one = mp::Dimension::none;
    REQUIRE(mp::dimension_from_name("ring", one));
    CHECK(one == mp::Dimension::ring);
    REQUIRE(mp::dimension_from_name("threads", one));
    CHECK(one == mp::Dimension::decoder_threads);
    CHECK_FALSE(mp::dimension_from_name("everything", one));

    const mp::Dimension both = mp::Dimension::ring | mp::Dimension::decoder_threads;
    CHECK(mp::has(both, mp::Dimension::ring));
    CHECK(mp::has(both, mp::Dimension::decoder_threads));
    CHECK_FALSE(mp::has(mp::Dimension::ring, mp::Dimension::decoder_threads));
    CHECK(mp::dimension_name(mp::Dimension::ring) == "ring");
    CHECK(mp::dimension_name(mp::Dimension::none).empty());
}

TEST_CASE("a profile survives being written and read back", "[buffering]")
{
    mp::Profile profile;
    mp::Measurement one = measured(uhd(), 96.0);
    one.decoder_threads = 8;
    one.file = "clips/forest.mkv";
    profile.measured.push_back(one);
    profile.measured.push_back(measured(hd(), 48.5));

    const std::string text = mp::write_profile(profile);
    const mp::ProfileText read = mp::parse_profile(text, "profile.ini");
    CHECK(read.complaints.empty());
    REQUIRE(read.profile.measured.size() == 2);
    CHECK(read.profile.measured[0].shape == uhd());
    CHECK(read.profile.measured[0].ring_ms == 96.0);
    CHECK(read.profile.measured[0].decoder_threads == 8);
    CHECK(read.profile.measured[0].file == "clips/forest.mkv");
    CHECK(read.profile.measured[0].runs == 2);
    CHECK(read.profile.measured[1].shape == hd());
    CHECK(read.profile.measured[1].ring_ms == 48.5);
}

TEST_CASE("a measurement that cannot be read loses its section and not the file",
          "[buffering]")
{
    const std::string text =
        "[measurement]\n"
        "codec = 0x00000101\n"
        "width = 3840\n"
        "height = 2160\n"
        "ring_ms = ninety-six\n"
        "\n"
        "[measurement]\n"
        "codec = 0x00000101\n"
        "width = 1920\n"
        "height = 1080\n"
        "ring_ms = 48\n";

    const mp::ProfileText read = mp::parse_profile(text, "profile.ini");
    // **Half a measurement is worse than none**: a class whose ring did not
    // read would put a zero where an answer belongs. The other class is
    // untouched, which is the whole reason a bad line does not fail the file.
    REQUIRE(read.profile.measured.size() == 1);
    CHECK(read.profile.measured[0].shape.width == 1920);
    CHECK(read.complaints.size() == 1);
}

TEST_CASE("a section nobody reads is said out loud rather than obeyed",
          "[buffering]")
{
    const mp::ProfileText read = mp::parse_profile("[player]\npath = processed\n",
                                                   "profile.ini");
    CHECK(read.profile.measured.empty());
    REQUIRE(read.complaints.size() == 1);
    CHECK(read.complaints[0].find("[measurement]") != std::string::npos);
}

TEST_CASE("a path with a comment character in it is left out, not mangled",
          "[buffering]")
{
    mp::Profile profile;
    mp::Measurement one = measured(uhd(), 96.0);
    one.file = "clips/take #2.mkv";
    profile.measured.push_back(one);

    const std::string text = mp::write_profile(profile);
    const mp::ProfileText read = mp::parse_profile(text, "profile.ini");
    CHECK(read.complaints.empty());
    REQUIRE(read.profile.measured.size() == 1);
    // The measurement survives; only the note about where it came from does not.
    CHECK(read.profile.measured[0].ring_ms == 96.0);
    CHECK(read.profile.measured[0].file.empty());
}

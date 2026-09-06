// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading back a rate the container could only round.
//
// The interesting half is the second one. That a nanosecond duration snaps to
// the rate that produced it is arithmetic; that a rate which is *genuinely* not
// on the list survives is the property that makes the first half a correction
// rather than a guess, and it is the one worth writing down.

#include "mediaperch/framerate.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

namespace {

/// What a muxer writes for `fps`: the frame's duration, rounded to a whole
/// nanosecond, which is all Matroska's `DefaultDuration` can hold.
std::uint64_t as_nanoseconds(mp::Rational fps)
{
    const double ns = 1.0e9 * fps.den / fps.num;
    return static_cast<std::uint64_t>(ns + 0.5);
}

/// And what a reader derives from that, the way `demux_mkv` does.
mp::Rational from_nanoseconds(std::uint64_t ns)
{
    return mp::Rational{1000000000u, static_cast<std::uint32_t>(ns)};
}

} // namespace

TEST_CASE("a rate that went through nanoseconds comes back", "[framerate]")
{
    // Every rate the correction knows, round-tripped through the encoding that
    // loses it. The loop is the test: a table of expected pairs would only say
    // that somebody typed the same numbers twice.
    for (const mp::Rational& rate : mp::standard_frame_rates()) {
        const std::uint64_t ns = as_nanoseconds(rate);
        const mp::Rational stated = from_nanoseconds(ns);
        bool snapped = false;
        const mp::Rational read = mp::snap_frame_rate(stated, &snapped);
        INFO(rate.num << "/" << rate.den << " -> " << ns << " ns -> " << stated.num << "/"
                      << stated.den);
        CHECK(read == rate);
        // `snapped` is false exactly when the encoding happened to be lossless,
        // which is every rate whose period is a whole number of nanoseconds.
        CHECK(snapped == !(stated == rate));
    }
}

TEST_CASE("the case that names the file", "[framerate]")
{
    // vp9.webm, which is what made this necessary: `show --match-refresh`
    // reported its 47.952 Hz match as inexact, at one slip per thirty days.
    const mp::Rational stated = from_nanoseconds(41708333);
    CHECK(stated.hz() == Catch::Approx(23.976024168).margin(1e-7));

    bool snapped = false;
    const mp::Rational read = mp::snap_frame_rate(stated, &snapped);
    CHECK(snapped);
    CHECK(read == mp::Rational{24000, 1001});
    CHECK(read.hz() == Catch::Approx(23.976023976).margin(1e-7));
}

TEST_CASE("a rate that is not a rounding is left alone", "[framerate]")
{
    // **This is the property that makes the correction honest.** 23.98 is
    // 1.7e-4 from 23.976 -- four thousand times the worst error the encoding
    // imposes -- so it is a different rate and not a rounded one.
    struct Odd {
        const char* what;
        mp::Rational stated;
    };
    const Odd odd[] = {
        {"23.98 exactly", mp::Rational{2398, 100}},
        {"23.9 exactly", mp::Rational{239, 10}},
        {"24.002", mp::Rational{24002, 1000}},
        {"a security camera at 7 fps", mp::Rational{7, 1}},
        // An MP4 written with a millisecond timescale: 23.976 stored as 42 ms,
        // which reads back as 23.810. Six parts in a thousand out, past any
        // threshold that can still tell 23.976 from 24, and genuinely
        // ambiguous -- so it stays as it is rather than being guessed at.
        {"a coarse millisecond timescale", mp::Rational{1000, 42}},
    };
    for (const Odd& one : odd) {
        bool snapped = true;
        const mp::Rational read = mp::snap_frame_rate(one.stated, &snapped);
        INFO(one.what << ": " << one.stated.hz());
        CHECK_FALSE(snapped);
        CHECK(read == one.stated);
    }
}

TEST_CASE("a rate a container states exactly is not touched", "[framerate]")
{
    // MP4 stores a timescale and a duration, so it can say the ratio. Nothing
    // to undo, and `snapped` says so -- which is how a caller tells "the file
    // meant this" from "the file said this".
    for (const mp::Rational& rate : mp::standard_frame_rates()) {
        bool snapped = true;
        const mp::Rational read = mp::snap_frame_rate(rate, &snapped);
        INFO(rate.num << "/" << rate.den);
        CHECK_FALSE(snapped);
        CHECK(read == rate);
    }

    // And a rate that is not stated at all stays not stated.
    bool snapped = true;
    CHECK_FALSE(mp::snap_frame_rate(mp::Rational{0, 0}, &snapped).valid());
    CHECK_FALSE(snapped);
}

TEST_CASE("the candidates are far enough apart for the threshold to mean something",
          "[framerate]")
{
    // The claim the header makes, checked rather than asserted: the worst
    // rounding is 4.0e-8 of the rate and the closest pair is 1.0e-3 apart.
    double worst_rounding = 0.0;
    for (const mp::Rational& rate : mp::standard_frame_rates()) {
        const mp::Rational stated = from_nanoseconds(as_nanoseconds(rate));
        worst_rounding =
            std::max(worst_rounding, std::abs(stated.hz() - rate.hz()) / rate.hz());
    }
    double closest_pair = 1.0;
    for (const mp::Rational& a : mp::standard_frame_rates()) {
        for (const mp::Rational& b : mp::standard_frame_rates()) {
            if (a == b) {
                continue;
            }
            closest_pair =
                std::min(closest_pair, std::abs(a.hz() - b.hz()) / std::max(a.hz(), b.hz()));
        }
    }
    INFO("worst rounding " << worst_rounding << ", closest pair " << closest_pair);
    CHECK(worst_rounding < 1.0e-7);
    CHECK(closest_pair > 9.0e-4);
    // The threshold sits between them with orders of magnitude either side,
    // which is the whole argument for it being a threshold at all.
    CHECK(worst_rounding < 1.0e-6);
    CHECK(1.0e-6 < closest_pair);
}

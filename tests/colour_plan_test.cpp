// SPDX-License-Identifier: GPL-3.0-or-later
//
// §9's decisions, checked without a display.
//
// **The HDR section of the plan is the one part of this project that could only
// ever be judged by looking at it**, and the whole reason it needs care is that
// looking at it is not a judgement: §9.2 records that the OS tone mapper is
// "good, free, and measurably wrong" -- it maps PQ to a 2.4 gamma rather than
// to the sRGB curve, and it has survived years of that because the result looks
// fine. Everything that follows from what the container said and what the
// display is, follows arithmetically, and that part can be held to the document
// that decided it.

#include "colour_plan.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

mp::video::Stream sdr_1080p()
{
    return mp::video::Stream{.primaries = mp::video::k_primaries_bt709,
                             .transfer = mp::video::k_transfer_bt709,
                             .matrix = mp::video::k_primaries_bt709,
                             .width = 1920,
                             .height = 1080};
}

mp::video::Stream hdr10_2160p()
{
    return mp::video::Stream{.primaries = mp::video::k_primaries_bt2020,
                             .transfer = mp::video::k_transfer_pq,
                             .matrix = mp::video::k_primaries_bt2020,
                             .width = 3840,
                             .height = 2160};
}

} // namespace

TEST_CASE("SDR content is not tone-mapped, whatever the display is", "[video][colour]")
{
    using namespace mp::video;

    // §9.1 asks how HDR reaches an SDR display. It does not ask anything about
    // SDR content, and mapping it anyway is how a player makes a picture that
    // was already right worse.
    for (const bool hdr_display : {false, true}) {
        Display display{};
        display.hdr = hdr_display;
        const Plan plan = plan_for(sdr_1080p(), display);
        CHECK(plan.tone_map == ToneMap::none);
        CHECK_FALSE(plan.tone_mapping);
        CHECK(plan.encoding == Encoding::linear);
    }
}

TEST_CASE("HDR content on an SDR display is mapped, because composition clips",
          "[video][colour]")
{
    using namespace mp::video;

    // **The finding §9.1 exists for.** Video processing tone-maps and DWM
    // composition clips, and a player that uploads its own texture and presents
    // it never went through the stage that honours the *Stream HDR video*
    // setting -- so nothing maps it and everything outside [0, 1] is simply
    // gone.
    Display sdr{};
    const Plan plan = plan_for(hdr10_2160p(), sdr);
    CHECK(plan.tone_mapping);
    CHECK(plan.encoding == Encoding::linear);

    // §9.3: the driver's is the default, because it is the cheapest, it is what
    // the OS player path uses, and it is the answer to why this looks like
    // Windows and MPC-BE does not.
    CHECK(plan.tone_map == ToneMap::driver);

    // And what a person asked for is honoured where it can be.
    CHECK(plan_for(hdr10_2160p(), sdr, ToneMap::shader).tone_map == ToneMap::shader);
    CHECK(plan_for(hdr10_2160p(), sdr, ToneMap::d2d).tone_map == ToneMap::d2d);

    // Asking for none on a display that needs one is asking for the clipping.
    // The plan declines: a request that would produce a knowingly wrong picture
    // is not a preference.
    CHECK(plan_for(hdr10_2160p(), sdr, ToneMap::none).tone_map == ToneMap::driver);
}

TEST_CASE("HDR content on an HDR display passes through", "[video][colour]")
{
    using namespace mp::video;

    Display hdr{};
    hdr.hdr = true;

    // Something composited over it -- subtitles, an OSD -- costs the cheaper
    // swap chain, because R10G10B10A2 cannot blend.
    const Plan with_osd = plan_for(hdr10_2160p(), hdr, ToneMap::driver, true);
    CHECK(with_osd.tone_map == ToneMap::none);
    CHECK_FALSE(with_osd.tone_mapping);
    CHECK(with_osd.encoding == Encoding::linear);
    CHECK(with_osd.needs_blending);

    // Nothing over it, and §9.5's fullscreen optimisation applies: half the
    // bandwidth at 4K, which is 24 MB a frame rather than 48.
    const Plan alone = plan_for(hdr10_2160p(), hdr, ToneMap::driver, false);
    CHECK(alone.encoding == Encoding::pq);
    CHECK_FALSE(alone.needs_blending);
}

TEST_CASE("SDR content on an HDR display is scaled to the display's white",
          "[video][colour]")
{
    using namespace mp::video;

    // **§9.6, the single most common HDR bug in players.** scRGB 1.0 is 80 nits
    // on an HDR display and is scene-referred; on an Advanced Color SDR display
    // it is the display's reference white. Subtitles drawn at 1.0 on the first
    // therefore arrive dim and grey next to the video.
    Display hdr{};
    hdr.hdr = true;
    hdr.sdr_white_nits = 240.0f;

    const Plan plan = plan_for(sdr_1080p(), hdr);
    CHECK(plan.sdr_scale == Catch::Approx(3.0)); // 240 / 80

    // The video needing no mapping does not mean the subtitles need no scaling,
    // so it applies to an HDR stream on an HDR display too.
    CHECK(plan_for(hdr10_2160p(), hdr).sdr_scale == Catch::Approx(3.0));

    // An SDR display already draws its own white at 1.0.
    Display sdr{};
    sdr.sdr_white_nits = 240.0f; // stated, and irrelevant
    CHECK(plan_for(sdr_1080p(), sdr).sdr_scale == Catch::Approx(1.0));

    // A display that says nothing is the scRGB reference, which is a scale of
    // exactly one rather than a guess.
    Display quiet{};
    quiet.hdr = true;
    CHECK(plan_for(sdr_1080p(), quiet).sdr_scale == Catch::Approx(1.0));

    // And nothing is divided by zero when it says something impossible.
    Display broken{};
    broken.hdr = true;
    broken.sdr_white_nits = 0.0f;
    CHECK(plan_for(sdr_1080p(), broken).sdr_scale == Catch::Approx(1.0));
}

TEST_CASE("a stream that states nothing gets the convention, not a guess",
          "[video][colour]")
{
    using namespace mp::video;

    // Code point 2 is "unspecified" in all three fields and is what a container
    // with no colour information leaves -- which the fixtures in tests/data
    // demonstrate is the common case, because FFmpeg wrote 2/2/2 for a file it
    // was explicitly told to tag as BT.709.
    Stream quiet{};
    quiet.width = 1920;
    quiet.height = 1080;
    CHECK(assumed_transfer(quiet) == k_transfer_bt709);
    CHECK(assumed_primaries(quiet) == k_primaries_bt709);

    // Below standard definition it is BT.601, which is the same convention
    // every player uses and which nothing writes down.
    Stream small{};
    small.width = 720;
    small.height = 480;
    CHECK(assumed_primaries(small) == 6u);

    // An unspecified stream is never treated as HDR, which is the safe
    // direction: tone-mapping SDR darkens it, and the failure is visible.
    Display sdr{};
    CHECK_FALSE(plan_for(quiet, sdr).tone_mapping);
}

TEST_CASE("HLG is HDR too, which is the transfer people forget", "[video][colour]")
{
    using namespace mp::video;

    // Broadcast HDR. It reaches a player from a stream rather than from a file
    // more often than PQ does, and a renderer that only checks for ST.2084
    // clips it exactly the way §9.1 describes.
    CHECK(is_hdr_transfer(k_transfer_hlg));
    CHECK(is_hdr_transfer(k_transfer_pq));
    CHECK_FALSE(is_hdr_transfer(k_transfer_bt709));
    CHECK_FALSE(is_hdr_transfer(k_transfer_srgb));
    CHECK_FALSE(is_hdr_transfer(k_transfer_unspecified));

    Stream hlg = hdr10_2160p();
    hlg.transfer = k_transfer_hlg;
    Display sdr{};
    CHECK(plan_for(hlg, sdr).tone_mapping);
}

TEST_CASE("HLG does not pass through, even on a display that can show it",
          "[video][colour]")
{
    using namespace mp::video;

    // **The bug this test was written for.** An earlier version of `plan_for`
    // put PQ and HLG in the same branch and, on an HDR display with nothing
    // composited, chose a PQ buffer for both with the comment "pass PQ
    // through". HLG is not PQ. PQ states absolute nits; HLG is scene-referred
    // and its OOTF is a system gamma derived from the display's peak. HLG code
    // values written into a buffer tagged PQ are a picture that is far too dark
    // and wrongly graded -- and it would have looked like a tone mapping fault.
    Display hdr{};
    hdr.hdr = true;
    hdr.peak_nits = 600.0f;

    Stream pq = hdr10_2160p();
    Stream hlg = hdr10_2160p();
    hlg.transfer = k_transfer_hlg;

    // PQ, alone on an HDR display, is the one genuine pass-through in §9.
    const Plan pq_plan = plan_for(pq, hdr, ToneMap::driver, false);
    CHECK(pq_plan.encoding == Encoding::pq);
    CHECK(pq_plan.convert == Convert::none);
    CHECK_FALSE(pq_plan.tone_mapping);

    // HLG is linearised instead, which costs the packed buffer. No platform
    // here presents HLG directly -- DXGI has no HLG swap chain colour space and
    // neither does CAMetalLayer -- so that is the honest price of the format
    // rather than a limitation of this module.
    const Plan hlg_plan = plan_for(hlg, hdr, ToneMap::driver, false);
    CHECK(hlg_plan.encoding == Encoding::linear);
    CHECK(hlg_plan.convert == Convert::hlg_to_linear);
    // And still not tone-mapped: the display can show every stop of it. The
    // conversion and the mapping are different questions, which is why they are
    // two fields.
    CHECK_FALSE(hlg_plan.tone_mapping);
    CHECK(hlg_plan.tone_map == ToneMap::none);

    // The OOTF needs the display's peak, so the plan carries it rather than
    // making a shader ask a display anything.
    CHECK(hlg_plan.hlg_peak_nits == Catch::Approx(600.0f));

    // On an SDR display it is both converted and mapped.
    Display sdr{};
    const Plan hlg_on_sdr = plan_for(hlg, sdr);
    CHECK(hlg_on_sdr.convert == Convert::hlg_to_linear);
    CHECK(hlg_on_sdr.tone_mapping);

    // And everything that is not HLG takes the ordinary linearisation.
    CHECK(plan_for(sdr_1080p(), sdr).convert == Convert::to_linear);
    CHECK(plan_for(pq, sdr).convert == Convert::to_linear);
}

TEST_CASE("the names a person types round-trip", "[video][colour]")
{
    using namespace mp::video;

    for (const ToneMap m : {ToneMap::none, ToneMap::driver, ToneMap::d2d, ToneMap::shader}) {
        ToneMap back{};
        REQUIRE(tone_map_from_name(name_of(m), back));
        CHECK(back == m);
    }
    ToneMap ignored{};
    CHECK_FALSE(tone_map_from_name("reinhard", ignored));
    CHECK_FALSE(tone_map_from_name(nullptr, ignored));
}

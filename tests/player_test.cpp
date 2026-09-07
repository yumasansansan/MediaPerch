// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine, with no operating system under it.
//
// `mp::Player` is the whole product and it is portable, so it can be tested the
// way the graph is: a fake device that records every byte, sources made of
// bytes a test chose, and no COM, no LoadLibrary and no audio hardware. What
// the Windows head adds -- opening a file with a decoder module, opening an
// endpoint -- is `IEngineHost`, and here it is twenty lines.

#include "fake_host.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using mp::test::cd_audio;
using mp::test::Host;
using mp::test::pattern;
using mp::test::wait_for;
using mp::test::wait_for_state;

namespace {

/// A display that never runs out, so the picture ends when the track does
/// rather than when the clock does.
class Endless final : public mp::IFrameClock {
public:
    bool wait() override
    {
        if (cancelled_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        now_ += 166'667;
        return !cancelled_.load(std::memory_order_acquire);
    }
    [[nodiscard]] double nominal_interval() const override { return 1.0 / 60.0; }
    [[nodiscard]] std::uint64_t now() const override { return now_; }
    [[nodiscard]] std::uint64_t rate() const override { return 10'000'000; }
    void cancel() noexcept override { cancelled_.store(true, std::memory_order_release); }

private:
    std::uint64_t now_ = 0;
    std::atomic<bool> cancelled_{false};
};

} // namespace

TEST_CASE("an engine opens the picture in a file and closes it with the track",
          "[player][video]")
{
    // **§9.7.1 reaching `Player`.** One file, opened once, with both halves
    // coming out of it (§4); a presenter opened through `IEngineHost`; a
    // decoder handed that presenter's graphics device (§9.8.1); and a display
    // loop on a thread of its own. None of it needs a window, a GPU or a
    // display, which is the whole reason the assembly moved into `src/player`.
    //
    // **What this test is not about is pacing.** Whether a frame is shown,
    // dropped or held is §8's arithmetic against a clock somebody drives, and
    // it is checked where a clock can be driven: avsync_test.cpp for the
    // decision, display_loop_test.cpp for the loop that makes it, and
    // video_path_test.cpp for the assembly running against one. Driving the
    // device's own clock from here would be a third copy of that, in the one
    // test where the device is being paced by something else.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        mp::test::decoder_log().frames = 100'000;
    }

    Host host;
    host.add("film", pattern(65536, 3));
    host.add_video("film");
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));

    // The presenter was opened and told what the container said about the
    // picture -- not about the audio, and not the decoder's own answer, which
    // arrives later and only if the bitstream disagrees.
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        CHECK(mp::test::presenter_log().info.width == 16u);
        CHECK(mp::test::presenter_log().info.height == 16u);
        CHECK(mp::test::presenter_log().info.timescale == 1000u);
    }
    // And a decoder was opened rather than merely chosen.
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        CHECK(mp::test::decoder_log().open);
    }

    // The track ends, and the picture is closed with it: the loop's thread
    // joined and the decoder shut, rather than left turning against a graph
    // that has gone.
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        return !mp::test::decoder_log().open;
    }));
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        CHECK_FALSE(mp::test::presenter_log().open);
    }
    CHECK(player.status().underruns == 0);

    player.shutdown();
}

TEST_CASE("a file with no picture plays exactly as it did", "[player][video]")
{
    // The ordinary case, and the one that must not have changed: nothing is
    // opened, nothing is presented, and the audio is what it was.
    mp::test::presenter_log().reset();

    Host host;
    host.add("song", pattern(4096, 4));
    mp::Player player{host};
    player.start();
    player.play({"song"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));

    const std::lock_guard lock{mp::test::presenter_log().mutex};
    CHECK_FALSE(mp::test::presenter_log().open);
    CHECK_FALSE(mp::test::presenter_log().configured);
    CHECK(mp::test::presenter_log().presented.load(std::memory_order_relaxed) == 0u);
    player.shutdown();
}

TEST_CASE("a picture that will not open is a track that still plays",
          "[player][video]")
{
    // **A player that got worse when it gained a feature** is the failure this
    // guards against. A machine with no presenter module is a machine that
    // plays the audio, and says once why there is nothing to look at.
    mp::test::presenter_log().reset();

    Host host;
    host.add("film", pattern(4096, 5));
    host.add_video("film");
    host.no_presenter();

    mp::Player player{host};
    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));

    CHECK(player.status().underruns == 0);
    CHECK(host.said("playing without the picture"));
    player.shutdown();
}

namespace {

/// A measurement for the class `TapeMedia` says its picture is, at a ring size
/// nothing else would choose, so a log line naming it can only have come from
/// here.
mp::Profile measured_profile(double ring_ms)
{
    mp::Measurement one;
    one.shape = mp::StreamShape{MP_CODEC_AV1, 16, 16, 25, 1};
    one.ring_ms = ring_ms;
    one.file = "a test";
    one.runs = 3;
    mp::Profile out;
    out.measured.push_back(one);
    return out;
}

} // namespace

TEST_CASE("a profile measured on this machine sizes the ring", "[player][profile]")
{
    // **§9.8.2 reaching the engine.** `show` has asked the profile since the
    // calibration was built; the engine could not, because the profile is keyed
    // on a class of *video* stream and there was no video path to have one. Now
    // there is.
    Host host;
    host.add("film", pattern(4096, 7));
    host.add_video("film");

    mp::Player player{host};
    // 100 ms of a 64-frame period at 44100 Hz is about 69 periods, which is
    // nothing like the default and could not have come from anywhere else.
    player.use_profile(measured_profile(100.0));
    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));

    CHECK(host.said("profile: "));
    CHECK(host.said("ring periods rather than"));
    player.shutdown();
}

TEST_CASE("a number somebody typed is not overruled by a measurement",
          "[player][profile]")
{
    // **The one direction this must not be wrong in.** §9.8.2's three answers
    // are in an order that respects who said what, and a profile that quietly
    // replaced a person's number would make the setting a suggestion.
    Host host;
    host.add("film", pattern(4096, 8));
    host.add_video("film");

    mp::Player player{host};
    player.use_profile(measured_profile(100.0));
    std::string why;
    REQUIRE(player.set("ring_periods", "40", why));
    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));

    CHECK_FALSE(host.said("profile: "));
    player.shutdown();
}

TEST_CASE("a track with no picture has no class to look up", "[player][profile]")
{
    // The profile is keyed on a class of video stream, so an audio-only track
    // is not a small measurement -- it is not a measurement at all, and asking
    // would be asking about the wrong thing.
    Host host;
    host.add("song", pattern(4096, 9));

    mp::Player player{host};
    player.use_profile(measured_profile(100.0));
    player.start();
    player.play({"song"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));

    CHECK_FALSE(host.said("profile: "));
    player.shutdown();
}

TEST_CASE("the shell says which display, and the engine tells the presenter",
          "[player][video]")
{
    // **§9.7.1's other message, end to end inside the engine.** A windowless
    // presenter falls back to the first output, which is a guess about which
    // monitor the picture is on; the shell has the window and says. What is
    // checked here is that it arrives -- what it then *decides* is
    // video_d3d11_test.cpp's, where the numbers come back out as pixels.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();

    Host host;
    // Long enough that the track is still playing when the second message
    // arrives: the fake device is paced far faster than real time, and a
    // half-second file is over before a test can say anything twice.
    host.add("film", pattern(2 * 1024 * 1024, 6));
    host.add_video("film");
    // A picture with nothing to pace it is a picture the engine drops, so the
    // second message would have nowhere to go. That is `start_video`'s rule
    // and it is right -- a presenter that cannot be paced draws nothing and
    // should not hold a graphics device open.
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    mp::VideoPath::DisplayIs display;
    display.hdr = true;
    display.white_nits = 480.0f;
    display.peak_nits = 600.0f;
    std::string why;
    // **Before anything is playing**, and it is remembered rather than
    // refused: a shell should not have to wait for a track to say where its
    // window is.
    REQUIRE(player.set_display(true, display, why));

    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));

    REQUIRE(wait_for([] {
        return !mp::test::presenter_log().setting("display").empty();
    }));
    CHECK(mp::test::presenter_log().setting("display") ==
          "hdr=1,wide=0,white=480.0000,peak=600.0000");

    // Said again while it is playing, which is a window crossing a monitor.
    display.hdr = false;
    display.white_nits = 240.0f;
    REQUIRE(player.set_display(true, display, why));
    CHECK(mp::test::presenter_log().setting("display") ==
          "hdr=0,wide=0,white=240.0000,peak=600.0000");

    // And stopping knowing is a message too, not a silence.
    REQUIRE(player.set_display(false, display, why));
    CHECK(mp::test::presenter_log().setting("display") == "probe");

    player.shutdown();
}

TEST_CASE("a display message with no picture to apply it to is still taken",
          "[player][video]")
{
    // A shell should not have to know whether the current track has video to
    // tell the engine where its window is, and it will be right for the next
    // one that does.
    mp::test::presenter_log().reset();

    Host host;
    host.add("song", pattern(4096, 10));
    mp::Player player{host};
    player.start();

    mp::VideoPath::DisplayIs display;
    display.hdr = true;
    std::string why;
    CHECK(player.set_display(true, display, why));
    CHECK(why.empty());
    player.shutdown();
}

TEST_CASE("an engine plays what it is told to", "[player]")
{
    Host host;
    host.add("one", pattern(4096, 1));
    mp::Player player{host};
    player.start();

    CHECK(player.status().state == mp::ipc::State::stopped);
    player.play({"one"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));

    const mp::ipc::Status playing = player.status();
    CHECK(playing.track == "one");
    CHECK(playing.decoder == "decode_test");
    CHECK(playing.device == "fake");
    CHECK(playing.count == 1);
    CHECK(playing.source == cd_audio());
    CHECK_FALSE(playing.processed);

    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    const mp::ipc::Status ended = player.status();
    CHECK(ended.underruns == 0);
    CHECK(ended.frames_rendered >= 4096 / mp::frame_bytes(cd_audio()));
    player.shutdown();
}

TEST_CASE("an engine says which track it could not open", "[player]")
{
    // Recorded rather than fatal: a playlist that silently plays four of its
    // five entries is worse than one that says which it skipped.
    Host host;
    host.add("good", pattern(2048, 2));
    mp::Player player{host};
    player.start();
    player.play({"good", "missing"});
    // Waiting for `playing` first, because `stopped` is also where it started:
    // a test that waits for the state it began in has not waited at all.
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return host.said("skipping missing"); }));
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    player.shutdown();
}

TEST_CASE("an engine takes transport commands from another thread", "[player]")
{
    Host host;
    // Long enough that the test can do things to it while it plays.
    host.add("long", pattern(64 * 4 * 200, 3));
    mp::Player player{host};
    player.start();
    player.play({"long"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));

    SECTION("pause stops the clock and resume starts it again")
    {
        REQUIRE(wait_for([&] { return player.status().position > 0; }));
        player.pause();
        CHECK(player.status().state == mp::ipc::State::paused);
        REQUIRE(wait_for([&] {
            const std::uint64_t a = player.status().position;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            return player.status().position == a;
        }));
        player.resume();
        CHECK(player.status().state == mp::ipc::State::playing);
    }

    SECTION("seek moves it")
    {
        REQUIRE(player.seek(1000, false));
        CHECK(player.status().position >= 1000);
        // Relative, from wherever it is now.
        const std::uint64_t before = player.status().position;
        REQUIRE(player.seek(-500, true));
        CHECK(player.status().position < before);
    }

    SECTION("seeking before the bottom stops at the bottom")
    {
        REQUIRE(player.seek(-100000, true));
        CHECK(player.status().position < 44100);
    }

    SECTION("stop ends it")
    {
        player.stop();
        REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    }

    player.shutdown();
}

TEST_CASE("an engine joins two tracks and can be told to skip one", "[player]")
{
    Host host;
    host.add("a", pattern(64 * 4 * 40, 4));
    host.add("b", pattern(64 * 4 * 40, 5));
    mp::Player player{host};
    player.start();
    player.play({"a", "b"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));

    // The queue is asked, not the graph, so the device never notices.
    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 1; }));
    CHECK(player.status().track == "b");
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    CHECK(player.status().underruns == 0);
    player.shutdown();
}

TEST_CASE("an engine refuses a setting it cannot make sense of", "[player]")
{
    Host host;
    mp::Player player{host};
    std::string why;

    CHECK_FALSE(player.set("nonsense", "1", why));
    CHECK(why.find("nonsense") != std::string::npos);
    CHECK_FALSE(player.set("path", "sideways", why));
    CHECK_FALSE(player.set("gain", "not a number", why));
    CHECK_FALSE(player.set("dither", "sprinkles", why));
    CHECK_FALSE(player.set("share", "sometimes", why));

    // **A number it cannot make sense of, and not a number it disapproves of.**
    // The rule is C++'s own, restated for the person at the other end: give the
    // user the choice even when the user may be wrong. So text with no number
    // in it is refused, a number too large for the field is refused because it
    // would arrive as a different number -- and everything else is taken.
    CHECK_FALSE(player.set("gain", "1 and a half", why));
    CHECK_FALSE(player.set("ring_periods", "-1", why));
    CHECK_FALSE(player.set("dither_seed", "1e30", why));
    CHECK_FALSE(player.set("gain", "inf", why));

    CHECK(player.set("gain", "1e9", why));         // will clip, and says so by clipping
    CHECK(player.set("gain", "-1", why));          // inverts, which is a thing to want
    CHECK(player.set("ring_periods", "1", why));   // the lowest latency a machine can do
    CHECK(player.set("ring_periods", "0", why));   // the ring sizer's own floor, then
    CHECK(player.set("ring_periods", "100000", why));
    CHECK(player.set("wait_timeout", "0", why));   // do not wait at all
    CHECK(player.set("recover_timeout", "86400", why));

    CHECK(player.set("path", "processed", why));
    CHECK(player.set("gain", "0.5", why));
    CHECK(player.set("dither", "none", why));
    CHECK(player.set("share", "shared", why));
    CHECK(player.set("recover", "off", why));

    // What it says back is what it was told, so a shell can show it.
    const auto settings = player.settings();
    const auto value = [&](const std::string& key) {
        const auto found = std::find_if(settings.begin(), settings.end(),
                                        [&](const auto& s) { return s.key == key; });
        return found == settings.end() ? std::string{"<missing>"} : found->value;
    };
    CHECK(value("path") == "processed");
    CHECK(value("dither") == "none");
    CHECK(value("share") == "shared");
    CHECK(value("recover") == "0");
    // Every row explains itself: a settings list a person cannot act on is a
    // list of guesses.
    for (const mp::ipc::Setting& s : settings) {
        INFO(s.key);
        CHECK_FALSE(s.description.empty());
    }
}

TEST_CASE("a setting that changes the graph rebuilds it where it stands", "[player]")
{
    Host host;
    host.add("long", pattern(64 * 4 * 300, 6));
    mp::Player player{host};
    player.start();
    player.play({"long"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return player.status().position > 500; }));

    const std::uint64_t before = player.status().position;
    std::string why;
    REQUIRE(player.set("path", "processed", why));

    // The device stops and starts -- that gap is real and exclusive mode has no
    // way around it. What is not real is a glitch: the next run begins where
    // the last one stopped, so nothing is played twice and nothing is skipped.
    REQUIRE(wait_for([&] { return player.status().processed; }));
    CHECK(player.status().position >= before);
    CHECK(player.status().state == mp::ipc::State::playing);
    player.shutdown();
}

TEST_CASE("a setting the device will not take is put back", "[player]")
{
    // Somebody asked for something impossible. Stopping the music would be the
    // easy answer and the wrong one.
    Host host;
    host.add("long", pattern(64 * 4 * 300, 7));
    mp::Player player{host};
    player.start();
    player.play({"long"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return player.status().position > 500; }));

    std::string why;
    REQUIRE(player.set("device", "one that is not there", why));
    REQUIRE(wait_for([&] { return host.said("put back"); }));
    CHECK(wait_for_state(player, mp::ipc::State::playing));
    CHECK(player.status().device == "fake");
    player.shutdown();
}

TEST_CASE("an engine waits for a device that was taken away", "[player][device]")
{
    Host host;
    host.add("long", pattern(64 * 4 * 400, 8));
    host.rules_.waits_before_loss = 20;
    mp::Player player{host};
    player.start();
    player.play({"long"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));

    // The device goes. Everything above it is still alive, so this is a
    // rebuild rather than an ending -- and the resume point is what the device
    // was given, not how far the decoder had read.
    REQUIRE(wait_for([&] { return host.said("the device went away"); }));
    REQUIRE(wait_for([&] { return host.said("resuming at frame"); }));
    CHECK(player.status().state == mp::ipc::State::playing);
    player.shutdown();
}

TEST_CASE("an engine that cannot find a device says so and stops", "[player][device]")
{
    Host host;
    host.add("one", pattern(2048, 9));
    host.unplug(true);
    mp::Player player{host};
    std::string why;
    REQUIRE(player.set("recover_timeout", "0", why));
    player.start();
    player.play({"one"});
    // Nothing ever plays here, so the state never leaves `stopped` and there is
    // nothing to wait for except the engine saying why.
    REQUIRE(wait_for([&] { return host.said("the device is not there"); }));
    CHECK(player.status().state == mp::ipc::State::stopped);
    CHECK_FALSE(player.status().error.empty());
    player.shutdown();
}

TEST_CASE("an engine can be shut down at any moment", "[player]")
{
    // Not a nicety: the process this lives in is killed by people closing
    // windows, and a shutdown that only works from a stopped state is one that
    // never gets exercised.
    Host host;
    host.add("long", pattern(64 * 4 * 200, 10));

    SECTION("before anything played") {
        mp::Player player{host};
        player.start();
        player.shutdown();
    }
    SECTION("while it is playing") {
        mp::Player player{host};
        player.start();
        player.play({"long"});
        REQUIRE(wait_for_state(player, mp::ipc::State::playing));
        player.shutdown();
    }
    SECTION("without ever being started") {
        mp::Player player{host};
        player.shutdown();
    }
    SECTION("twice") {
        mp::Player player{host};
        player.start();
        player.shutdown();
        player.shutdown();
    }
    SUCCEED("it came back");
}

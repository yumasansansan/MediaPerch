// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine, with no operating system under it.
//
// `mp::Player` is the whole product and it is portable, so it can be tested the
// way the graph is: a fake device that records every byte, sources made of
// bytes a test chose, and no COM, no LoadLibrary and no audio hardware. What
// the Windows head adds -- opening a file with a decoder module, opening an
// endpoint -- is `IEngineHost`, and here it is twenty lines.

#include "fake_dsp.hpp"
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

/// It lives with the other fakes now, because ipc_test.cpp needs one too:
/// the picture surviving a shell is a claim about the door as much as about
/// the player.
using Endless = mp::test::EndlessClock;

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

TEST_CASE("a file with no audio plays its picture on the video engine's own clock",
          "[player][video]")
{
    // **Two engines, two clocks, and here only one of them.** A file with a
    // picture and no sound is not a skipped entry: the player runs it without
    // a device or a graph, the picture paces on `VideoPath::own_clock`, the
    // position is that clock's milliseconds, and the run ends when the
    // picture does.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        // Thirty frames, forty milliseconds apart: 1.2 s of picture.
        mp::test::decoder_log().frames = 30;
    }
    Host host;
    host.add_silent("clip", 1200);
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"clip"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    CHECK(player.status().track == "clip");
    CHECK(player.status().clock_rate == 1000u);
    CHECK(player.status().length == 1200u);
    CHECK(player.status().device.empty());
    CHECK(player.status().source.sample_rate == 0u);
    REQUIRE(wait_for([&] { return host.said("on the picture's own clock"); }));
    REQUIRE(wait_for([&] { return player.status().item_position >= 100u; }));

    // Ends when the picture does, with nothing to say.
    REQUIRE(wait_for([&] { return player.status().state == mp::ipc::State::stopped; }, 8000));
    CHECK(player.status().error.empty());
    CHECK(player.status().underruns == 0u);
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        return !mp::test::decoder_log().open;
    }));
    player.shutdown();
}

TEST_CASE("a playlist walks from a song into a silent picture and out again",
          "[player][video]")
{
    // The queue stops in front of the picture as it stops in front of another
    // format; the picture plays on its own clock; a queue begins again after
    // it. Three entries, three runs, one playlist.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        mp::test::decoder_log().frames = 20;
    }
    Host host;
    host.add("song", pattern(4096, 2));
    host.add_silent("clip", 800);
    host.add("last", pattern(4096, 3));
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"song", "clip", "last"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return host.said("playing clip on the picture's own clock"); }, 8000));
    REQUIRE(wait_for([&] { return player.status().track == "last"; }, 8000));
    CHECK(player.status().clock_rate == 44100u);
    REQUIRE(wait_for([&] { return player.status().state == mp::ipc::State::stopped; }, 8000));
    CHECK(player.status().error.empty());
    CHECK(host.said("the next entry has no audio in it"));
    player.shutdown();
}

TEST_CASE("a picture on its own clock pauses, seeks, and steps between entries",
          "[player][video]")
{
    // The transport with no graph under it: pause stops the clock, resume
    // starts it, a seek moves the file and the anchor, and next and previous
    // end the run and walk. The fake frame clock runs sixteen times faster
    // than the wall, so the numbers below are the clock's and not the test's.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        mp::test::decoder_log().frames = 1'000'000;
    }
    Host host;
    host.add_silent("clip", 60'000);
    host.add_silent("second", 60'000);
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"clip", "second"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return player.status().item_position >= 50u; }));

    player.pause();
    REQUIRE(wait_for_state(player, mp::ipc::State::paused));
    const std::uint64_t held = player.status().item_position;
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK(player.status().item_position == held);
    player.resume();
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return player.status().item_position > held; }));

    // A seek lands where it was asked, in the clock's own milliseconds.
    REQUIRE(player.seek(5000, false));
    REQUIRE(wait_for([&] {
        const std::uint64_t at = player.status().item_position;
        return at >= 5000u && at < 7000u;
    }));

    // Previous from well in is the start of this one; next is the one after;
    // previous from just after arriving is the one before.
    player.previous();
    REQUIRE(wait_for([&] { return player.status().item_position < 1000u; }));
    CHECK(player.status().track == "clip");
    player.next();
    REQUIRE(wait_for([&] { return player.status().track == "second"; }));
    CHECK(player.status().index == 1u);
    player.previous();
    REQUIRE(wait_for([&] { return player.status().track == "clip"; }));

    player.stop();
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    CHECK(player.status().error.empty());
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

TEST_CASE("a track boundary does not rebuild the picture", "[player][video]")
{
    // **§8 does not rebuild the audio device at a boundary -- not rebuilding
    // it is what gapless is -- and the picture now follows the same rule.**
    //
    // It did not, and nothing looked wrong until there was a shell. The
    // presenter is where the composition surface lives, so a new presenter is
    // a new handle: a shell had to detach and attach at every boundary, which
    // on a playlist of one-second files is a black frame and then an empty one,
    // once a second. What changes at a boundary is the file, not the display.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();

    Host host;
    host.add("one", pattern(1024 * 1024, 6));
    host.add_video("one");
    host.add("two", pattern(1024 * 1024, 7));
    host.add_video("two");
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"one", "two"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));

    std::string why;
    REQUIRE(player.set_node("presenter", "size", "640x360", why));
    CHECK(mp::test::presenter_log().setting("size") == "640x360");

    // **What a shell compares.** The surface handle is duplicated afresh into
    // the asking process on every ask, so it is a different number each time
    // and cannot answer *is this the one I already have*. This can.
    const std::uint64_t first = player.picture_generation();
    CHECK(first != 0u);

    // **The boundary asked for rather than waited for.** `next` is the same
    // path a track ending takes -- the queue is asked and the picture follows
    // it -- and it happens when the test says so instead of when a fake device
    // has drained a megabyte.
    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 1; }));
    // The decoder is opened again, because the codec may have changed. That is
    // how this test knows the boundary has actually been crossed.
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        return mp::test::decoder_log().opens >= 2u;
    }));

    // And the presenter was not.
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        CHECK(mp::test::presenter_log().opens == 1u);
        CHECK(mp::test::presenter_log().open);
    }
    CHECK(player.picture_generation() == first);
    // Which is why the size is still what it was: nothing had to remember it.
    CHECK(mp::test::presenter_log().setting("size") == "640x360");

    // And `native` puts it back, rather than being a size nothing can express.
    REQUIRE(player.set_node("presenter", "size", "native", why));
    CHECK(mp::test::presenter_log().setting("size") == "native");

    player.shutdown();
}

TEST_CASE("a track with no picture ends it, and the next one gets its size back",
          "[player][video]")
{
    // **The boundary that does rebuild.** A file with no video ends the
    // picture -- there is nothing to draw and a stale last frame would be a
    // lie -- so the presenter closes and the next file that has one opens a
    // new presenter, which knows nothing. That is the case `Player` remembers
    // the shell's size for, and it is the one it was found by: a queue of
    // one-second files and `node presenter` answering `native`.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();

    Host host;
    host.add("seen", pattern(1024 * 1024, 6));
    host.add_video("seen");
    host.add("heard", pattern(64 * 4 * 40, 7)); // no picture
    host.add("seen again", pattern(1024 * 1024, 8));
    host.add_video("seen again");
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"seen", "heard", "seen again"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));

    std::string why;
    REQUIRE(player.set_node("presenter", "size", "640x360", why));
    const std::uint64_t first = player.picture_generation();

    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 1; }));
    // No picture at all, which a shell draws as one.
    REQUIRE(wait_for([&] { return player.surface() == 0; }));
    // The presenter closes a moment later: `surface` answers zero as soon as
    // the engine thread has let go of the path, and the path is destroyed by
    // whoever drops the last reference to it.
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return !mp::test::presenter_log().open;
    }));

    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 2; }));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().opens >= 2u;
    }));
    REQUIRE(wait_for([] {
        return !mp::test::presenter_log().setting("size").empty();
    }));
    // A different picture, and the size the shell asked for is on it. Waited
    // for rather than checked: the presenter's own log fills inside `open`,
    // and the generation counts at the end of it, a moment later.
    REQUIRE(wait_for([&] { return player.picture_generation() > first; }));
    CHECK(mp::test::presenter_log().setting("size") == "640x360");

    player.shutdown();
}

TEST_CASE("a size the presenter refuses is not remembered", "[player][video]")
{
    // **trust the user, up to what the hardware can hold.** A shell may ask
    // for anything; what a shell must not do is have a refusal quietly become
    // the size every later track opens at.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();

    Host host;
    host.add("one", pattern(1024 * 1024, 6));
    host.add_video("one");
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    player.play({"one"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        mp::test::presenter_log().refuse_size = true;
    }

    std::string why;
    CHECK_FALSE(player.set_node("presenter", "size", "999999x999999", why));
    CHECK_FALSE(why.empty());

    player.shutdown();
}

TEST_CASE("a chain round-trips through the settings surface, settings and all",
          "[player][settings]")
{
    // **One separator was doing two jobs.** Stages were joined with the comma
    // that also separates a stage's own settings, so a stage with two keys
    // came back through `set dsp` as two stages. The canvas rewrites the whole
    // string, so this is the grammar it relies on: `|` between stages, and the
    // old comma form still read for the settings files that have it.
    Host host;
    host.add_dsp("dsp_test", &mp::test::fake_dsp_vtbl());
    mp::Player player{host};
    std::string why;

    // Two stages, the new way; what comes back is what was said.
    REQUIRE(player.set("dsp", "test:amount=2|test:amount=3", why));
    REQUIRE(player.node_settings("dsp.0")[0].value == "2");
    REQUIRE(player.node_settings("dsp.1")[0].value == "3");
    const auto row_of = [&](const char* key) {
        for (const mp::ipc::Setting& s : player.settings()) {
            if (s.key == key) {
                return s.value;
            }
        }
        return std::string{};
    };
    CHECK(row_of("dsp") == "test:amount=2|test:amount=3");

    // A stage's second setting stays with its stage, and comes back joined the
    // unambiguous way -- which is what `save` writes and `set` reads again.
    REQUIRE(player.set("dsp", "test:amount=2,amount=5", why));
    CHECK(player.node_settings("dsp.0")[0].value == "5");
    CHECK(player.node_settings("dsp.1").empty());
    REQUIRE(player.set("dsp", row_of("dsp"), why));
    CHECK(player.node_settings("dsp.0")[0].value == "5");
    CHECK(player.node_settings("dsp.1").empty());

    // The old form: two stages separated by a comma, each with a key.
    REQUIRE(player.set("dsp", "test:amount=7,test:amount=8", why));
    CHECK(player.node_settings("dsp.0")[0].value == "7");
    CHECK(player.node_settings("dsp.1")[0].value == "8");
    CHECK(row_of("dsp") == "test:amount=7|test:amount=8");

    // And a set through the node, then the whole string read back.
    REQUIRE(player.set_node("dsp.1", "amount", "9", why));
    REQUIRE(player.set("dsp", row_of("dsp"), why));
    CHECK(player.node_settings("dsp.1")[0].value == "9");
}

TEST_CASE("a measurement crosses as one, not as two English words",
          "[player][settings]")
{
    // **§10 carries the difference, so no shell has to parse a description.**
    // A module marks a row it will not take back by ending its description
    // with `(read only)`; that is read once, where the rows are parsed, and
    // what a shell gets is a boolean. A shell that looked for the words would
    // be a second reader of a convention, and the one that drifted.
    Host host;
    host.add_dsp("dsp_test", &mp::test::fake_dsp_vtbl());

    mp::Player player{host};
    std::string why;
    REQUIRE(player.set("dsp", "test:amount=2", why));

    const std::vector<mp::ipc::Setting> rows = player.node_settings("dsp.0");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "amount");
    CHECK_FALSE(rows[0].read_only);
    CHECK(rows[1].key == "peak");
    CHECK(rows[1].read_only);

    // And it survives the wire, which is the half a shell actually sees.
    mp::ipc::Writer w;
    write(w, rows);
    mp::ipc::Reader r{w.bytes().data(), w.bytes().size()};
    std::vector<mp::ipc::Setting> back;
    REQUIRE(read(r, back));
    REQUIRE(r.complete());
    REQUIRE(back.size() == 2);
    CHECK_FALSE(back[0].read_only);
    CHECK(back[1].read_only);
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

TEST_CASE("the engine measures this machine and keeps the answer",
          "[player][calibrate]")
{
    // **§9.8.2's calibration, run by the engine.** §10 has carried the verb
    // since the driver was written; what the engine could not do was assemble
    // the A/V graph a run measures, and now it can. A calibration takes the
    // machine over -- what was playing stops -- because measuring beside a
    // playlist would be measuring a machine that is doing something else.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();

    Host host;
    host.add("film", pattern(64 * 1024, 11));
    host.add_video("film");
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    player.start();
    CHECK(player.profile_text().find("[measurement]") == std::string::npos);

    mp::CalibrationPlan plan;
    // Two rings and one window each, which is four seconds of nothing and a
    // fifth of a second of this. `ring_order` gives powers of two because
    // `ByteRing` rounds to one and the sizes between are the same ring.
    plan.sweep = mp::Sweep::every;
    plan.lowest_ring = 4;
    plan.highest_ring = 8;
    plan.windows.count = 1;
    plan.windows.seconds = 0.05;
    player.calibrate({"film"}, plan);

    REQUIRE(wait_for([&] { return host.said("measured "); }));
    CHECK(host.said("measuring 1 file"));

    // Applied as well as reported: a measurement this machine made about itself
    // is the answer to the question the default is a guess at.
    const std::string text = player.profile_text();
    CHECK(text.find("[measurement]") != std::string::npos);
    // And the presenter was in the path, which is what makes it a measurement
    // of *video* rather than of an audio graph with a name.
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        CHECK(mp::test::presenter_log().info.width == 16u);
    }
    player.shutdown();
}

TEST_CASE("a file with no picture in it is skipped, not failed",
          "[player][calibrate]")
{
    // The profile is keyed on a class of video stream, so a file with none has
    // no class to be measured against. Named, and the calibration goes on.
    Host host;
    host.add("song", pattern(4096, 12));

    mp::Player player{host};
    player.start();

    mp::CalibrationPlan plan;
    plan.windows.count = 1;
    plan.windows.seconds = 0.05;
    player.calibrate({"song"}, plan);

    REQUIRE(wait_for([&] { return host.said("skipped song"); }));
    CHECK(host.said("no video in it"));
    CHECK(player.profile_text().find("[measurement]") == std::string::npos);
    player.shutdown();
}

namespace {

/// The node with this id, or null. A canvas asks by id and so does a test.
const mp::ipc::Node* node_of(const mp::ipc::Graph& graph, const std::string& id)
{
    for (const mp::ipc::Node& node : graph.nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

bool joined(const mp::ipc::Graph& graph, const std::string& from, const std::string& to)
{
    for (const mp::ipc::Edge& edge : graph.edges) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("the engine says what shape it is in", "[player][graph]")
{
    // **§10's node canvas, from the engine's side.** The shape is derived every
    // time it is asked for rather than kept beside the graph, so what a shell
    // draws is what would be built and not a model of it that has drifted.
    Host host;
    host.add("one", pattern(4096, 20));
    mp::Player player{host};

    // **Path A: source to sink, and nothing insertable between them.** §5 is
    // why -- a `memcpy` or a container repack has no stage in it, and a canvas
    // that drew a converter there would be drawing something that is not there.
    {
        const mp::ipc::Graph shape = player.graph();
        CHECK(node_of(shape, "source") != nullptr);
        CHECK(node_of(shape, "sink") != nullptr);
        CHECK(node_of(shape, "convert") == nullptr);
        CHECK(joined(shape, "source", "sink"));
    }

    // Asking for Path B puts the converter in, before anything is playing:
    // a canvas has to be drawable before there is a run to draw.
    std::string why;
    REQUIRE(player.set("path", "processed", why));
    {
        const mp::ipc::Graph shape = player.graph();
        const mp::ipc::Node* convert = node_of(shape, "convert");
        REQUIRE(convert != nullptr);
        CHECK(convert->kind == static_cast<std::uint32_t>(mp::ipc::NodeKind::convert));
        // It is not a module, and says so by having no module id: the f64 bus,
        // the dither and the shaping are arithmetic in the core.
        CHECK(convert->module.empty());
        CHECK(joined(shape, "source", "convert"));
        CHECK(joined(shape, "convert", "sink"));
    }
}

TEST_CASE("a stage is a node a person may move, and the rest are not",
          "[player][graph]")
{
    // What a canvas may rearrange is what a person assembled. Everything else
    // on the line is decided by §5 and §6 and is there whether anybody wants it
    // or not, so it is not removable and dragging it should not be offered.
    Host host;
    host.add_dsp("dsp_test", &mp::test::fake_dsp_vtbl());
    mp::Player player{host};

    std::string why;
    REQUIRE(player.set("dsp", "test,test:amount=3", why));

    const mp::ipc::Graph shape = player.graph();
    const mp::ipc::Node* first = node_of(shape, "dsp.0");
    const mp::ipc::Node* second = node_of(shape, "dsp.1");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->module == "dsp_test");
    CHECK((first->flags & mp::ipc::MP_NODE_REMOVABLE) != 0u);
    CHECK((first->flags & mp::ipc::MP_NODE_SETTABLE) != 0u);

    // **In the order they run**, which is the one thing a chain has that a set
    // does not, and the reason the canvas is a line rather than a cloud.
    CHECK(joined(shape, "source", "dsp.0"));
    CHECK(joined(shape, "dsp.0", "dsp.1"));
    // A chain forces Path B, so the converter is there without anybody asking.
    CHECK(joined(shape, "dsp.1", "convert"));
    CHECK(joined(shape, "convert", "sink"));

    const mp::ipc::Node* sink = node_of(shape, "sink");
    REQUIRE(sink != nullptr);
    CHECK((sink->flags & mp::ipc::MP_NODE_REMOVABLE) == 0u);
}

TEST_CASE("a node answers with its own settings, and takes one", "[player][graph]")
{
    // **The settings button.** Today the only way to change a stage's parameter
    // is to rewrite the whole `dsp` spec string, which a GUI would have to
    // reassemble from what it thinks the chain is. This is that, keyed by node.
    Host host;
    host.add_dsp("dsp_test", &mp::test::fake_dsp_vtbl());
    mp::Player player{host};

    std::string why;
    REQUIRE(player.set("dsp", "test", why));

    // **Every key the module has, not only the ones somebody set.** Asked of a
    // stage opened for the question rather than of one that is playing, which
    // is also why this works before anything plays.
    const std::vector<mp::ipc::Setting> rows = player.node_settings("dsp.0");
    REQUIRE_FALSE(rows.empty());
    const auto has = [&rows](const std::string& key, const std::string& value) {
        return std::any_of(rows.begin(), rows.end(), [&](const mp::ipc::Setting& row) {
            return row.key == key && row.value == value;
        });
    };
    CHECK(has("amount", "1"));

    REQUIRE(player.set_node("dsp.0", "amount", "7", why));
    CHECK(player.node_settings("dsp.0")[0].value == "7");
    // And the spec string that a settings file round-trips through moved with
    // it, because there is one place the chain is written down.
    const std::vector<mp::ipc::Setting> all = player.settings();
    CHECK(std::any_of(all.begin(), all.end(), [](const mp::ipc::Setting& row) {
        return row.key == "dsp" && row.value.find("amount=7") != std::string::npos;
    }));

    // A key the stage does not have is refused, and the chain is put back --
    // a canvas that half-applied a setting would be a canvas showing a chain
    // the engine does not have.
    CHECK_FALSE(player.set_node("dsp.0", "nonsense", "1", why));
    CHECK_FALSE(why.empty());
    CHECK(player.node_settings("dsp.0")[0].value == "7");

    // And a node that is not there says so rather than doing nothing.
    CHECK_FALSE(player.set_node("dsp.9", "amount", "1", why));
    CHECK(player.node_settings("dsp.9").empty());
}

TEST_CASE("the palette is every module the host loaded", "[player][graph]")
{
    Host host;
    host.add_module(mp::ipc::ModuleRow{3u, "dsp_gain", "Gain", 100u, true});
    host.add_module(mp::ipc::ModuleRow{9u, "vdsp_lut", "Lookup table", 100u, true});
    mp::Player player{host};

    const std::vector<mp::ipc::ModuleRow> rows = player.modules();
    REQUIRE(rows.size() == 2u);
    CHECK(rows[0].id == "dsp_gain");
    // **The kind goes as its number**, so a shell that has never heard of
    // MP_KIND_VDSP still lists it rather than dropping it.
    CHECK(rows[1].kind == 9u);
}

TEST_CASE("a video chain is opened, handed over, and drawn as nodes",
          "[player][video][graph]")
{
    // **§9.8.3 reaching the canvas.** A video stage opens on the presenter's
    // device, is handed to the presenter rather than run beside it, and appears
    // between the decoder and the presenter -- which is where it *is*, even
    // though it runs inside the presenter's two halves. That last point is the
    // one a canvas has to draw honestly.
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    mp::test::stage_log().reset();

    Host host;
    host.add("film", pattern(64 * 1024, 21));
    host.add_video("film");
    host.add_video_dsp("vdsp_test", &mp::test::fake_video_stage_vtbl());
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    std::string why;
    REQUIRE(player.set("video_dsp", "test,test:amount=3", why));

    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().stages == 2u;
    }));
    {
        const std::lock_guard lock{mp::test::stage_log().mutex};
        CHECK(mp::test::stage_log().opened == 2u);
        CHECK(mp::test::stage_log().configured);
        // The spec's settings reached it before it was handed over, so nothing
        // is ever shown through a stage that has not been told what it is.
        CHECK(mp::test::stage_log().amount == "3");
    }

    // **The nodes, in the order they run.** The presenter is the end of the
    // line even though the chain runs inside it: what a canvas draws is where
    // a stage sits in the picture's path, not which object owns the pass.
    const mp::ipc::Graph shape = player.graph();
    const auto has_edge = [&shape](const std::string& from, const std::string& to) {
        return std::any_of(shape.edges.begin(), shape.edges.end(),
                           [&](const mp::ipc::Edge& e) {
                               return e.from == from && e.to == to;
                           });
    };
    CHECK(has_edge("vsource", "vdsp.0"));
    CHECK(has_edge("vdsp.0", "vdsp.1"));
    CHECK(has_edge("vdsp.1", "presenter"));

    // And the settings button works on one, live -- which for a video stage is
    // the loop's hold rather than a rebuild, because there is one device and
    // one presenter and no opening a second for the question.
    const std::vector<mp::ipc::Setting> rows = player.node_settings("vdsp.0");
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].key == "amount");
    REQUIRE(player.set_node("vdsp.0", "amount", "9", why));
    {
        const std::lock_guard lock{mp::test::stage_log().mutex};
        CHECK(mp::test::stage_log().amount == "9");
    }
    // A key the stage does not have is refused with the module named.
    CHECK_FALSE(player.set_node("vdsp.0", "nonsense", "1", why));
    CHECK(why.find("vdsp_test") != std::string::npos);

    player.shutdown();
    // Every stage that was opened was closed, and before the presenter that was
    // holding them.
    const std::lock_guard lock{mp::test::stage_log().mutex};
    CHECK(mp::test::stage_log().closed == mp::test::stage_log().opened);
}

TEST_CASE("a video stage that will not open leaves a picture that still plays",
          "[player][video]")
{
    // The same rule the audio side has and the presenter has: a run that got
    // worse when it gained a feature is the failure to guard against.
    mp::test::presenter_log().reset();
    mp::test::stage_log().reset();
    {
        const std::lock_guard lock{mp::test::stage_log().mutex};
        mp::test::stage_log().refuse_open = true;
    }

    Host host;
    host.add("film", pattern(4096, 22));
    host.add_video("film");
    host.add_video_dsp("vdsp_test", &mp::test::fake_video_stage_vtbl());
    host.pace_with([] { return std::make_unique<Endless>(); });

    mp::Player player{host};
    std::string why;
    REQUIRE(player.set("video_dsp", "test", why));
    player.start();
    player.play({"film"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([] {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        return mp::test::presenter_log().configured;
    }));

    // The picture is there and the chain is empty, which is one pass.
    {
        const std::lock_guard lock{mp::test::presenter_log().mutex};
        CHECK(mp::test::presenter_log().stages == 0u);
    }
    CHECK(player.status().underruns == 0);
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

TEST_CASE("an engine says which track it could not open, and plays the rest", "[player]")
{
    // Recorded rather than fatal: a playlist that silently plays four of its
    // five entries is worse than one that says which it skipped -- and one
    // that stops at the second because it would not open is worse again,
    // which is what this did until the queue learned to walk past.
    Host host;
    host.add("good", pattern(2048, 2));
    host.add("last", pattern(2048, 3));
    mp::Player player{host};
    player.start();
    player.play({"good", "missing", "last"});
    // Waiting for `playing` first, because `stopped` is also where it started:
    // a test that waits for the state it began in has not waited at all.
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return host.said("skipping missing"); }));
    REQUIRE(wait_for([&] { return player.status().track == "last"; }));
    CHECK(player.status().index == 2);
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    CHECK(player.status().error.empty());
    CHECK(player.status().underruns == 0);
    player.shutdown();
}

TEST_CASE("a playlist with nothing that opens says why, in the words of whoever refused it",
          "[player]")
{
    // What three video-only WebMs dropped on the shell produced was *the
    // playlist has nothing at 0*, which is true and says nothing; the engine
    // log had *no audio track in it* all along. The run's error is the
    // playlist's own line now: the last entry refused, by name, and the count.
    Host host;
    mp::Player player{host};
    player.start();
    player.play({"C:\\somewhere\\first.mkv", "C:\\somewhere\\second.mkv"});
    REQUIRE(wait_for([&] { return !player.status().error.empty(); }));
    CHECK(player.status().error ==
          "2 entries would not open; the last, second.mkv: no decoder recognised it");
    CHECK(player.status().state == mp::ipc::State::stopped);
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

TEST_CASE("next starts the following track now, from where the listener is", "[player]")
{
    // The button, end to end: the ring is thrown away and refilled from the
    // next track's start, the device is fed silence for exactly that long, and
    // nothing underruns. Two tracks long enough that the decoder is well ahead.
    Host host;
    host.add("a", pattern(64 * 4 * 4000, 4));
    host.add("b", pattern(64 * 4 * 4000, 5));
    mp::Player player{host};
    player.start();
    player.play({"a", "b"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return player.status().position > 2000; }));
    const auto before = player.status();
    REQUIRE(before.index == 0);

    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 1; }));
    const auto after = player.status();
    CHECK(after.state == mp::ipc::State::playing);
    CHECK(after.track == "b");
    // From its start, and the queue's clock did not jump backwards to get there.
    CHECK(after.item_position < 64 * 4000 / 4);
    CHECK(after.position >= before.position);
    CHECK(after.underruns == 0);

    // A next on the last track ends the run, now rather than after the rest.
    player.next();
    REQUIRE(wait_for_state(player, mp::ipc::State::stopped));
    CHECK(player.status().underruns == 0);
    player.shutdown();
}

TEST_CASE("a track in another format reopens the device and plays on", "[player]")
{
    // **The one join a queue will not make, made by the player instead.** The
    // queue stops at a boundary whose next track has a different format and
    // says which; the run ends, the device is reopened for that format, and
    // the next run starts on that track. It did not: the run's end looked like
    // the playlist's, and the player said stopped.
    mp::Format other = cd_audio();
    other.sample_rate = 48000;
    Host host;
    host.add("one", pattern(64 * 4 * 200, 4));
    host.add("two", pattern(64 * 4 * 4000, 5), other);
    mp::Player player{host};
    player.start();
    player.play({"one", "two"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(player.status().source.sample_rate == 44100u);

    // The second track, in its own format, without anybody pressing anything.
    REQUIRE(wait_for([&] {
        const auto s = player.status();
        return s.index == 1 && s.state == mp::ipc::State::playing;
    }, 8000));
    CHECK(player.status().source.sample_rate == 48000u);
    CHECK(player.status().track == "two");
    CHECK(player.status().underruns == 0);
    player.shutdown();
}

TEST_CASE("an entry the engine has not reached can move while the rest plays", "[player]")
{
    // **What a drag in the playlist means.** The run's playlist is a list of
    // paths opened lazily by the decode thread, so an entry past the decoder is
    // free to change places; one at or before it is playing, in the ring, or
    // already marked, and moving it would move the ground the run stands on.
    Host host;
    host.add("a", pattern(64 * 4 * 4000, 4));
    host.add("b", pattern(64 * 4 * 4000, 5));
    host.add("c", pattern(64 * 4 * 4000, 6));
    host.add("d", pattern(64 * 4 * 4000, 7));
    mp::Player player{host};
    std::string why;

    // Nothing playing: any order.
    player.play({"a", "b", "c", "d"});
    player.start();
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    REQUIRE(player.status().index == 0);

    // Playing "a": "c" and "d" may swap; "a" may not go anywhere.
    REQUIRE(player.move_entry(2, 3, why));
    CHECK(player.playlist() == std::vector<std::string>{"a", "b", "d", "c"});
    CHECK_FALSE(player.move_entry(0, 3, why));
    CHECK(why.find("already read") != std::string::npos);
    CHECK_FALSE(player.move_entry(3, 0, why));
    CHECK(player.playlist() == std::vector<std::string>{"a", "b", "d", "c"});
    CHECK_FALSE(player.move_entry(1, 9, why));

    // And the queue plays the new order: skip to what is now third.
    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 1; }));
    player.next();
    REQUIRE(wait_for([&] { return player.status().index == 2; }));
    CHECK(player.status().track == "d");
    CHECK(player.status().underruns == 0);
    player.shutdown();
}

TEST_CASE("a click on a track starts the run there", "[player]")
{
    // **Not a seek and not a string of nexts.** A queue records where a track
    // began as it goes past it, so it cannot place one it has not reached; and
    // each next opens a file and none of them is atomic. What a click means is
    // the run starting again at that entry, which is a real gap and is what
    // the person asked for.
    Host host;
    host.add("a", pattern(64 * 4 * 4000, 4));
    host.add("b", pattern(64 * 4 * 4000, 5));
    host.add("c", pattern(64 * 4 * 4000, 6));
    mp::Player player{host};
    player.start();
    player.play({"a", "b", "c"});
    REQUIRE(wait_for_state(player, mp::ipc::State::playing));
    CHECK(player.status().index == 0);

    std::string why;
    REQUIRE(player.play_at(2, why));
    REQUIRE(wait_for([&] {
        const auto s = player.status();
        return s.state == mp::ipc::State::playing && s.index == 2;
    }));
    CHECK(player.status().track == "c");
    CHECK(player.status().count == 3);

    // An entry the playlist does not have is refused with the count in it.
    CHECK_FALSE(player.play_at(3, why));
    CHECK(why.find("3 entries") != std::string::npos);
    CHECK(player.status().index == 2);

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

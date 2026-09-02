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
#include <string>
#include <vector>

using mp::test::cd_audio;
using mp::test::Host;
using mp::test::pattern;
using mp::test::wait_for;
using mp::test::wait_for_state;

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
    CHECK_FALSE(player.set("gain", "1e9", why));
    CHECK_FALSE(player.set("dither", "sprinkles", why));
    CHECK_FALSE(player.set("share", "sometimes", why));
    CHECK_FALSE(player.set("ring_periods", "1", why));

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

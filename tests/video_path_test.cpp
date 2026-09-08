// SPDX-License-Identifier: GPL-3.0-or-later
//
// The video path, assembled on a machine with no display.
//
// **This used to be untestable, and that is the point of moving it.** The
// assembly -- open a presenter, hand it to a decoder, build the graph, run the
// loop -- lived inside one command in the Windows head, where reaching it meant
// a window, a swap chain and a file. It is `mp::VideoPath` in `src/player` now,
// which means the two doors it goes through are `IEngineHost`'s and a test can
// be the operating system.
//
// What is checked here is the wiring and the order, not the pixels. Pixels are
// video_d3d11_test.cpp and hdr_transfer_test.cpp, which render and read back on
// WARP; a fake presenter can only say what it was *asked*.

#include "fake_host.hpp"
#include "fake_video.hpp"

#include "mediaperch/video_path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using mp::test::decoder_log;
using mp::test::presenter_log;

namespace {

MpVideoInfo picture(std::uint32_t width, std::uint32_t height)
{
    MpVideoInfo out{};
    out.size = sizeof(out);
    out.width = width;
    out.height = height;
    out.display_width = width;
    out.display_height = height;
    out.timescale = 1000;
    out.fps_num = 25;
    out.fps_den = 1;
    return out;
}

/// A feed with a packet always ready, because what is under test is the
/// assembly and not the router. `PacketRouter` is packet_test.cpp's.
class Endless final : public mp::IPacketFeed {
public:
    MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) override
    {
        buffer.assign(16, 0x5A);
        out = MpPacket{};
        out.size = sizeof(out);
        out.bytes = static_cast<std::uint32_t>(buffer.size());
        out.frame = frame_;
        frame_ += 40;
        return MP_OK;
    }

private:
    std::uint64_t frame_ = 0;
};

/// A frame clock that gives `turns` turns and then stops -- or stops early,
/// because that is what `cancel` is and what `VideoPath::stop` sends.
class CountedFrames final : public mp::IFrameClock {
public:
    explicit CountedFrames(std::uint64_t turns) noexcept : left_(turns) {}

    bool wait() override
    {
        if (cancelled_.load(std::memory_order_acquire) || left_ == 0) {
            return false;
        }
        --left_;
        now_ += 166'667; // a 60 Hz refresh, at the rate below
        return true;
    }
    [[nodiscard]] double nominal_interval() const override { return 1.0 / 60.0; }
    [[nodiscard]] std::uint64_t now() const override { return now_; }
    [[nodiscard]] std::uint64_t rate() const override { return 10'000'000; }
    void cancel() noexcept override
    {
        cancelled_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_acquire);
    }

private:
    std::uint64_t left_;
    std::uint64_t now_ = 0;
    std::atomic<bool> cancelled_{false};
};

/// A clock that never runs out, for the tests about stopping one that is still
/// turning. It sleeps a millisecond a turn so the thread is genuinely inside
/// `wait` rather than spinning through it.
class SlowFrames final : public mp::IFrameClock {
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
    void cancel() noexcept override
    {
        cancelled_.store(true, std::memory_order_release);
    }

private:
    std::uint64_t now_ = 0;
    std::atomic<bool> cancelled_{false};
};

/// A clock to follow that has stopped: the loop still turns, and it turns with
/// nothing to decide against.
class NoClock final : public mp::IMediaClock {
public:
    [[nodiscard]] mp::ClockSpec spec() const override { return {}; }
    bool read(mp::ClockReading&) override { return false; }
};

/// Everything the fakes hold, put back between tests -- they are static
/// because the ABI hands out opaque handles, and a test that inherited the
/// previous one's counters would be a test about the order the file is in.
struct Fresh {
    Fresh()
    {
        presenter_log().reset();
        decoder_log().reset();
    }
};

} // namespace

TEST_CASE("the video path opens the presenter first, because the decoder wants its device",
          "[video][path]")
{
    const Fresh fresh;
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    mp::VideoPath::Config want;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, want,
                      why));
    CHECK(why.empty());
    CHECK(path.opened());

    // §9.8.1's order, which is the reason `open` cannot be written the other
    // way round: the presenter is configured -- that is where a real one makes
    // its device -- before anything asks it for one.
    {
        const std::lock_guard lock{presenter_log().mutex};
        CHECK(presenter_log().open);
        CHECK(presenter_log().configured);
        CHECK(presenter_log().info.width == 64u);
        CHECK(presenter_log().info.height == 48u);
    }
    {
        const std::lock_guard lock{decoder_log().mutex};
        CHECK(decoder_log().open);
    }

    // Named, so a report can say which. A run that says which decoder it used
    // and not which presenter is half a report.
    CHECK(path.modules().presenter == "video_test");
    CHECK(path.modules().decoder == "vcodec_test");
}

TEST_CASE("a size the caller asked for is set before the picture is configured",
          "[video][path]")
{
    // **§9.7.1's decision, one layer up.** The presenter takes `size` before
    // `configure` because that is where its target is made; this is the code
    // that has to know it.
    const Fresh fresh;
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    mp::VideoPath::Config want;
    want.width = 1280;
    want.height = 720;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, want,
                      why));

    CHECK(presenter_log().setting("size") == "1280x720");
    {
        const std::lock_guard lock{presenter_log().mutex};
        // The picture is still the picture: a size to render at is not a claim
        // about what the file contains, and `configure` gets the container's
        // numbers whatever the target is.
        CHECK(presenter_log().info.width == 64u);
        REQUIRE(presenter_log().settings.size() >= 1u);
        CHECK(presenter_log().settings.front().first == "size");
    }
}

TEST_CASE("a size the presenter will not render at is a failure, not a shrug",
          "[video][path]")
{
    const Fresh fresh;
    {
        const std::lock_guard lock{presenter_log().mutex};
        presenter_log().refuse_size = true;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    mp::VideoPath::Config want;
    want.width = 1280;
    want.height = 720;
    std::string why;
    // **A shell that asked for a size and silently got another one would
    // composite the wrong rectangle**, so this says so rather than carrying on
    // at the picture's own size.
    CHECK(!path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, want,
                     why));
    CHECK(why.find("1280x720") != std::string::npos);
    CHECK(!path.opened());
    // And nothing is left half-open behind it.
    {
        const std::lock_guard lock{decoder_log().mutex};
        CHECK(!decoder_log().open);
    }
}

TEST_CASE("a thread count reaches the decoder, and a refusal reaches the caller",
          "[video][path]")
{
    const Fresh fresh;
    mp::test::Host host;
    Endless feed;

    mp::VideoPath::Config want;
    want.decoder_threads = 6;

    {
        mp::VideoPath path;
        std::string why;
        REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0,
                          want, why));
        const std::lock_guard lock{decoder_log().mutex};
        CHECK(decoder_log().threads == "6");
    }

    // MP_ERR_BUSY is what a decoder says when it has already started its
    // workers. A number nobody used would make a calibration sweep measure the
    // same thing several times and call the results different.
    decoder_log().reset();
    {
        const std::lock_guard lock{decoder_log().mutex};
        decoder_log().refuse_threads = true;
    }
    mp::VideoPath path;
    std::string why;
    CHECK(!path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, want,
                     why));
    CHECK(why.find("6 threads") != std::string::npos);
}

TEST_CASE("a machine with no presenter and one with no decoder both say which",
          "[video][path]")
{
    const Fresh fresh;
    Endless feed;
    mp::VideoPath::Config want;

    {
        mp::test::Host host;
        host.no_presenter();
        mp::VideoPath path;
        std::string why;
        CHECK(!path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0,
                         want, why));
        CHECK(why == "no presenter module is loaded");
    }
    {
        mp::test::Host host;
        host.no_video_codec();
        mp::VideoPath path;
        std::string why;
        CHECK(!path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0,
                         want, why));
        CHECK(why == "nothing here decodes that video codec");
    }
}

TEST_CASE("the loop runs on a thread of its own and ends when the display does",
          "[video][path]")
{
    const Fresh fresh;
    {
        const std::lock_guard lock{decoder_log().mutex};
        // Enough that the loop always has one to hand, so the turns are about
        // the clock rather than about the decoder.
        decoder_log().frames = 10'000;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, {},
                      why));

    NoClock audio;
    CountedFrames frames{50};
    REQUIRE(path.start(&audio, frames, why));
    CHECK(path.running());
    // Starting twice is a mistake and is refused rather than leaking a thread.
    CHECK(!path.start(&audio, frames, why));
    // Following somebody else's clock, the engine's own is not made.
    CHECK(path.own_clock() == nullptr);

    REQUIRE(mp::test::wait_for([&] { return path.ended(); }));
    path.stop();
    CHECK(!path.running());

    // Fifty turns, and no more: the clock is what says when to stop, and the
    // loop asked it exactly as many times as it had answers.
    CHECK(path.loop().stats().turns == 50u);
    // A stopped clock, so every one of them was a turn with nothing to decide
    // against -- not an error, and not what a file with no audio gets either:
    // that runs on the engine's own clock, in the test below.
    CHECK(path.loop().stats().without_clock == 50u);
}

TEST_CASE("a path with nothing to follow runs on the video engine's own clock",
          "[video][path]")
{
    // **The video engine alone.** Started with no clock to follow, the path
    // makes a `FreeClock` over its frame clock's counter and the picture goes
    // up against that: every turn has a clock, frames are shown, and the
    // clock's position is the frame clock's elapsed time.
    const Fresh fresh;
    {
        const std::lock_guard lock{decoder_log().mutex};
        decoder_log().frames = 10'000;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, {},
                      why));

    CountedFrames frames{50};
    REQUIRE(path.start(nullptr, frames, why));
    REQUIRE(path.own_clock() != nullptr);
    REQUIRE(mp::test::wait_for([&] { return path.ended(); }));
    path.stop();

    CHECK(path.loop().stats().turns == 50u);
    CHECK(path.loop().stats().without_clock == 0u);
    CHECK(path.graph_stats().shown > 0u);
    // Fifty turns of a sixtieth of a second, in the clock's milliseconds.
    const std::uint64_t at = path.own_clock()->position();
    CHECK(at >= 800u);
    CHECK(at <= 900u);
}

TEST_CASE("a decoder that does not state a colour does not overrule the container",
          "[video][path][colour]")
{
    // **"Unspecified" is not speaking.** The container said PQ on BT.2020; a
    // VP9 bitstream names the family and not the curve, and a decoder that
    // answered 2 -- unspecified -- used to have that taken as a statement and
    // the container's PQ replaced with it: an HDR film drawn through an SDR
    // curve, flat and dark. Where a decoder does state a curve, it still wins.
    const auto run_with = [](std::uint32_t decoder_transfer) {
        const Fresh fresh;
        {
            const std::lock_guard lock{decoder_log().mutex};
            decoder_log().frames = 10;
            decoder_log().says = picture(64, 48);
            decoder_log().says.primaries = 9;
            decoder_log().says.transfer = decoder_transfer;
            decoder_log().says.matrix = 2;
        }
        mp::test::Host host;
        Endless feed;
        MpVideoInfo container = picture(64, 48);
        container.primaries = 9;
        container.transfer = 16; // PQ
        container.matrix = 9;

        mp::VideoPath path;
        std::string why;
        REQUIRE(path.open(host, nullptr, feed, container, MP_CODEC_AV1, nullptr, 0, {}, why));
        CountedFrames frames{40};
        REQUIRE(path.start(nullptr, frames, why));
        REQUIRE(mp::test::wait_for([&] { return path.ended(); }));
        path.stop();
        REQUIRE(path.graph_stats().decoded > 0u);
        const std::lock_guard lock{presenter_log().mutex};
        return presenter_log().info;
    };

    const MpVideoInfo kept = run_with(2);
    CHECK(kept.transfer == 16u);
    CHECK(kept.primaries == 9u);
    CHECK(kept.matrix == 9u);

    const MpVideoInfo overruled = run_with(14);
    CHECK(overruled.transfer == 14u);
    CHECK(overruled.primaries == 9u);
}

TEST_CASE("a seek on the engine's own clock pre-rolls without dropping, and holds the clock",
          "[video][path]")
{
    // A seek lands on the sync point before its target; the fake decoder,
    // reset, starts again from frame zero, so a seek to two seconds has fifty
    // frames of pre-roll at forty milliseconds each. They are let go and
    // counted as pre-roll, not as dropped; the clock stands at the target
    // until the first frame past them is in hand; and a clock somebody had
    // paused stays paused through it.
    const Fresh fresh;
    {
        const std::lock_guard lock{decoder_log().mutex};
        decoder_log().frames = 100'000;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, {},
                      why));
    SlowFrames frames;
    REQUIRE(path.start(nullptr, frames, why));
    REQUIRE(mp::test::wait_for([&] { return path.own_clock()->position() >= 200u; }));
    const std::uint64_t dropped_before = path.graph_stats().dropped;

    bool moved = false;
    REQUIRE(path.seek_alone(2.0, [&](double seconds) { moved = seconds == 2.0; return true; },
                            why));
    CHECK(moved);
    REQUIRE(mp::test::wait_for([&] { return path.graph_stats().preroll == 50u; }));
    REQUIRE(mp::test::wait_for([&] { return !path.own_clock()->paused(); }));
    REQUIRE(mp::test::wait_for([&] { return path.own_clock()->position() > 2000u; }));
    CHECK(path.graph_stats().dropped == dropped_before);
    REQUIRE(mp::test::wait_for([&] { return path.graph_stats().shown > 60u; }));

    // Paused, then seeked: still paused, and the target's frame was shown.
    path.own_clock()->pause();
    const std::uint64_t shown_before = path.graph_stats().shown;
    REQUIRE(path.seek_alone(4.0, [](double) { return true; }, why));
    REQUIRE(mp::test::wait_for([&] { return path.graph_stats().preroll == 150u; }));
    REQUIRE(mp::test::wait_for([&] { return path.graph_stats().shown > shown_before; }));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(path.own_clock()->paused());
    CHECK(path.own_clock()->position() == 4000u);
    path.stop();
}

TEST_CASE("stopping cancels the clock rather than waiting for it", "[video][path]")
{
    // **A loop is stopped from outside it.** `wait` blocks for a whole refresh
    // and a shutdown that waited for one would be a shutdown that took as long
    // as the display felt like -- which is why `IFrameClock` grew `cancel`.
    const Fresh fresh;
    {
        const std::lock_guard lock{decoder_log().mutex};
        decoder_log().frames = 10'000;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, {},
                      why));

    NoClock audio;
    SlowFrames frames;
    REQUIRE(path.start(&audio, frames, why));
    REQUIRE(mp::test::wait_for([&] { return path.loop().stats().turns > 2u; }));
    CHECK(!path.ended());

    path.stop();
    CHECK(!path.running());
    CHECK(path.ended());
    // Twice, because a caller that stops on the way out and again in a
    // destructor is the ordinary case.
    path.stop();
}

TEST_CASE("a resize reaches the presenter while the loop is turning", "[video][path]")
{
    // **§9.7.1's message, arriving mid-run.** A window dragged by a corner is
    // this call forty times a second, and a resize taken without holding the
    // loop would be two threads inside one graphics context.
    const Fresh fresh;
    {
        const std::lock_guard lock{decoder_log().mutex};
        decoder_log().frames = 10'000;
    }
    mp::test::Host host;
    Endless feed;

    mp::VideoPath path;
    std::string why;
    REQUIRE(path.open(host, nullptr, feed, picture(64, 48), MP_CODEC_AV1, nullptr, 0, {},
                      why));
    // Nothing was asked for, so nothing was said: a caller that wants the
    // picture's own size sends no message at all.
    CHECK(presenter_log().setting("size").empty());

    NoClock audio;
    SlowFrames frames;
    REQUIRE(path.start(&audio, frames, why));
    REQUIRE(mp::test::wait_for([&] { return path.loop().stats().turns > 2u; }));

    REQUIRE(path.set_size(800, 600, why));
    CHECK(presenter_log().setting("size") == "800x600");
    // The loop was let go again, so it is turning and deciding afterwards -- a
    // hold that was not released is a picture that stops updating. Waited for
    // rather than read: `parked` is the loop's acknowledgement and it is
    // cleared on the loop's next turn, not by `release` returning.
    const std::uint64_t turns = path.loop().stats().turns;
    REQUIRE(mp::test::wait_for([&] { return path.loop().stats().turns > turns; }));
    REQUIRE(mp::test::wait_for([&] { return !path.loop().parked(); }));

    // Zero is `native`, spelled the way the presenter's setting takes it.
    REQUIRE(path.set_size(0, 0, why));
    CHECK(presenter_log().setting("size") == "native");

    path.stop();
}

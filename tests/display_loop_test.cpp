// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop that drives the picture, with no display and no sound card.
//
// **Both clocks are numbers here**, which is the whole reason `DisplayLoop`
// takes them rather than reads them: a display that refreshes when the test
// says so, and an audio device whose position the test chooses. What is under
// test is the policy -- when to read, when to reconfigure, and what to do when
// there is no clock at all -- and none of that can be arranged on hardware.

#include "mediaperch/display.hpp"

#include "mediaperch/packet.hpp"
#include "module_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using mp::test::Module;

namespace {

constexpr std::uint64_t k_tick_rate = 10'000'000;

/// A display that refreshes when it is told to, a fixed number of times.
class CountedFrames final : public mp::IFrameClock {
public:
    CountedFrames(std::uint64_t turns, std::uint64_t step) noexcept
        : left_(turns), step_(step)
    {
    }

    bool wait() override
    {
        if (left_ == 0) {
            return false;
        }
        --left_;
        ticks_ += step_;
        return true;
    }
    [[nodiscard]] std::uint64_t now() const override { return ticks_; }
    [[nodiscard]] std::uint64_t rate() const override { return k_tick_rate; }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

private:
    std::uint64_t left_;
    std::uint64_t step_;
    std::uint64_t ticks_ = 0;
};

/// An audio device whose position the test writes.
class Dial final : public mp::IAudioClockSource {
public:
    explicit Dial(std::uint32_t rate = 48000) noexcept
    {
        spec_.wire_rate = rate;
        spec_.source_rate = rate;
    }

    [[nodiscard]] mp::ClockSpec spec() const override { return spec_; }
    bool read(mp::ClockReading& out) override
    {
        ++reads_;
        if (!running_) {
            return false;
        }
        out = reading_;
        return true;
    }

    /// Where the device is, and when it said so.
    void set(std::uint64_t frames, std::uint64_t ticks) noexcept
    {
        reading_.device_frames = frames;
        reading_.ticks = ticks;
        running_ = true;
    }
    void silence() noexcept { running_ = false; }
    /// A seek: the anchor moves.
    void anchor(std::uint64_t device_frame, std::uint64_t source_frame) noexcept
    {
        spec_.origin_device_frame = device_frame;
        spec_.origin_source_frame = source_frame;
    }
    [[nodiscard]] std::uint64_t reads() const noexcept { return reads_; }

private:
    mp::ClockSpec spec_{};
    mp::ClockReading reading_{};
    bool running_ = false;
    std::uint64_t reads_ = 0;
};

/// A feed that never runs out, so the graph never finishes and the loop keeps
/// turning for as long as the test wants it to.
class Endless final : public mp::IPacketFeed {
public:
    MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) override
    {
        buffer.assign(1, 0u);
        out = MpPacket{};
        out.size = sizeof(out);
        out.bytes = 1;
        return MP_OK;
    }
};

/// A decoder that hands back one frame per packet, timestamped so far ahead
/// that it is never due.
///
/// **Which is the state a player is in most of the time.** A frame is decoded,
/// it is not its turn yet, and the loop turns without presenting anything --
/// so this is what the clock tests below should be running against, rather
/// than a stream that ends on the first pump.
MpVideoCodec* fake_handle() noexcept
{
    static int one = 0;
    return reinterpret_cast<MpVideoCodec*>(&one);
}

MpResult MP_CALL fake_open(MpCodec, const MpGraphicsDevice*, const std::uint8_t*,
                           std::uint32_t, MpVideoCodec** out) noexcept
{
    *out = fake_handle();
    return MP_OK;
}
void MP_CALL fake_close(MpVideoCodec*) noexcept {}
MpResult MP_CALL fake_get_format(MpVideoCodec*, MpVideoInfo*) noexcept
{
    // Nothing to reconcile: the container's description is the only one.
    return MP_ERR_BUSY;
}
std::uint64_t g_pending = 0;
MpResult MP_CALL fake_decode(MpVideoCodec*, const void*, std::size_t,
                             std::uint64_t) noexcept
{
    ++g_pending;
    return MP_OK;
}
MpResult MP_CALL fake_next_frame(MpVideoCodec*, MpVideoFrame* out) noexcept
{
    if (g_pending == 0) {
        return MP_END;
    }
    --g_pending;
    out->width = 16;
    out->height = 16;
    out->layout = MP_LAYOUT_NV12;
    // A thousand seconds in, at the timescale these tests use. Never due, so
    // every pump is a repeat and nothing is ever presented -- which is what
    // lets these run with a presenter that was never opened.
    out->pts = 1000u * 1000u;
    return MP_OK;
}
MpResult MP_CALL fake_flush(MpVideoCodec*) noexcept { return MP_OK; }
MpResult MP_CALL fake_reset(MpVideoCodec*) noexcept
{
    g_pending = 0;
    return MP_OK;
}

const MpVideoCodecVtbl& fake_vtbl()
{
    static const MpVideoCodecVtbl vtbl{sizeof(MpVideoCodecVtbl),
                                       0,
                                       nullptr, /* probe */
                                       &fake_open,
                                       &fake_close,
                                       &fake_get_format,
                                       &fake_decode,
                                       &fake_next_frame,
                                       &fake_flush,
                                       &fake_reset};
    return vtbl;
}

/// Everything a loop needs except the two clocks.
struct Standing {
    Endless feed;
    mp::VideoDecoder decoder;
    mp::Presenter presenter;
    MpVideoInfo info{};
    mp::VideoGraph graph;

    Standing()
        : info(make_info()), graph(feed, decoder, presenter, info)
    {
        g_pending = 0;
        REQUIRE(decoder.open(fake_vtbl(), MP_CODEC_AV1, nullptr, nullptr, 0) == MP_OK);
    }

    static MpVideoInfo make_info() noexcept
    {
        MpVideoInfo out{};
        out.size = sizeof(out);
        out.width = 16;
        out.height = 16;
        out.timescale = 1000;
        out.fps_num = 25;
        out.fps_den = 1;
        return out;
    }
};

} // namespace

TEST_CASE("without a clock nothing is drawn, and it is not an error",
          "[display][avsync]")
{
    // A graph that has not started, or a sink module with no `get_position`.
    // §8 says everything follows the audio clock; when there is none, what
    // follows is the picture that is already up.
    Standing standing;
    mp::VideoGraph& graph = standing.graph;

    Dial dial;
    CountedFrames frames{10, k_tick_rate / 60};
    mp::DisplayLoop loop{graph, dial, frames};

    mp::DisplayStep step;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(loop.once(step));
        CHECK_FALSE(step.had_clock);
        CHECK(step.step == mp::VideoGraph::Step::repeated);
    }
    CHECK(loop.stats().turns == 5);
    CHECK(loop.stats().without_clock == 5);
    // The device was asked every turn. A loop that gave up after one refusal
    // would never notice the device starting.
    CHECK(dial.reads() == 5);
    // And the graph was never pumped, so it never reached the end of its empty
    // feed: nothing was decided against a guess.
    CHECK_FALSE(graph.finished());
}

TEST_CASE("the frame clock stopping stops the loop", "[display]")
{
    Standing standing;
    mp::VideoGraph& graph = standing.graph;

    Dial dial;
    CountedFrames frames{7, k_tick_rate / 60};
    mp::DisplayLoop loop{graph, dial, frames};

    CHECK(loop.run() == 7);
    CHECK(loop.stats().turns == 7);
}

TEST_CASE("the clock is read against the tick the frame is drawn at",
          "[display][avsync]")
{
    // One clock, not two. The tick a frame is drawn at is the tick the audio
    // position is extrapolated to, and taking them from separate sources would
    // put a scheduling delay between them -- which is a video frame's worth at
    // 60 Hz if the thread is unlucky.
    Standing standing;
    mp::VideoGraph& graph = standing.graph;

    Dial dial;
    CountedFrames frames{4, k_tick_rate}; // a second per turn, for legibility
    mp::DisplayLoop loop{graph, dial, frames};

    // The device said it had played a second, at the tick of the first turn.
    dial.set(48000, k_tick_rate);
    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    CHECK(step.had_clock);
    CHECK(loop.clock().audible_seconds(frames.ticks()) == 1.0);

    // A turn later the device has said nothing new, and the clock has run on
    // by exactly the turn.
    REQUIRE(loop.once(step));
    CHECK(loop.clock().audible_seconds(frames.ticks()) == 2.0);
}

TEST_CASE("a seek moves the anchor and the loop notices", "[display][avsync]")
{
    // The graphs move `played_base_` and `rendered_base_` when a seek lands,
    // and a loop that had configured its clock once would go on answering for
    // the run before it.
    Standing standing;
    mp::VideoGraph& graph = standing.graph;

    Dial dial;
    CountedFrames frames{6, k_tick_rate};
    mp::DisplayLoop loop{graph, dial, frames};

    dial.set(48000, k_tick_rate);
    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    CHECK(loop.clock().audible_seconds(frames.ticks()) == 1.0);
    // Configuring the first time is not a re-anchor: there was nothing to move
    // away from.
    CHECK(loop.stats().reanchored == 0);

    // A seek to ten minutes in, landing on the audio that had been written.
    dial.anchor(48000, 48000 * 600);
    dial.set(48000, 2 * k_tick_rate);
    REQUIRE(loop.once(step));
    CHECK(loop.stats().reanchored == 1);
    CHECK(loop.clock().audible_seconds(frames.ticks()) == 600.0);

    // And a turn with nothing moved does not count as another.
    dial.set(48000 + 48000, 3 * k_tick_rate);
    REQUIRE(loop.once(step));
    CHECK(loop.stats().reanchored == 1);
    CHECK(loop.clock().audible_seconds(frames.ticks()) == 601.0);
}

TEST_CASE("a device that goes quiet stops the picture rather than the loop",
          "[display][avsync]")
{
    // A sink that stops answering -- a device being pulled out, mid-run. The
    // loop keeps turning, keeps asking, and draws nothing against a clock it
    // no longer has.
    Standing standing;
    mp::VideoGraph& graph = standing.graph;

    Dial dial;
    CountedFrames frames{20, k_tick_rate / 60};
    mp::DisplayLoop loop{graph, dial, frames};

    dial.set(48000, 0);
    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    CHECK(step.had_clock);

    dial.silence();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(loop.once(step));
        // **Still had a clock**: the last reading is still the best answer
        // there is, and it is only wrong by however long the device has been
        // quiet. A loop that forgot it would blank the picture on one missed
        // read.
        CHECK(step.had_clock);
    }
    CHECK(loop.stats().without_clock == 0);
    CHECK(dial.reads() == 6);
}

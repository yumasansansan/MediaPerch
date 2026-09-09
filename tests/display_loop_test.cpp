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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <future>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
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

/// A display that refreshes at a fixed rate and is *observed* through noise.
///
/// **This is the clock the old estimator got wrong**, and it is the only kind
/// there is: the display's own interval is a crystal and does not wobble, but
/// the instant a thread notices a vertical blank does. So the tick returned is
/// the true grid plus a bounded, symmetric, zero-mean offset -- and the mean of
/// the gaps is exactly the interval while the minimum of them is not.
///
/// The noise is a small deterministic cycle rather than a generator, so the run
/// is the same every time and a failure is a failure rather than a seed.
class JitteryFrames final : public mp::IFrameClock {
public:
    /// `miss` makes every nth turn skip a refresh, which is what a turn that
    /// took too long looks like from here.
    JitteryFrames(std::uint64_t turns, std::uint64_t step, std::uint64_t swing,
                  std::uint64_t miss = 0) noexcept
        : left_(turns), step_(step), swing_(swing), miss_(miss)
    {
    }

    bool wait() override
    {
        if (left_ == 0) {
            return false;
        }
        --left_;
        ++index_;
        std::uint64_t refreshes = 1;
        if (miss_ != 0 && index_ % miss_ == 0) {
            refreshes = 3; // two vertical blanks went by unnoticed
        }
        grid_ += refreshes * step_;
        // A four-phase cycle summing to zero: +swing, 0, -swing, 0.
        static constexpr int k_phase[] = {1, 0, -1, 0};
        const int phase = k_phase[index_ % 4];
        offset_ = static_cast<std::int64_t>(swing_) * phase;
        return true;
    }
    [[nodiscard]] std::uint64_t now() const override
    {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(grid_) + offset_);
    }
    [[nodiscard]] std::uint64_t rate() const override { return k_tick_rate; }
    [[nodiscard]] double nominal_interval() const override { return 0.0; }

private:
    std::uint64_t left_;
    std::uint64_t step_;
    std::uint64_t swing_;
    std::uint64_t miss_;
    std::uint64_t index_ = 0;
    std::uint64_t grid_ = 1'000'000;
    std::int64_t offset_ = 0;
};

/// A clock to follow whose position the test writes: an audio device, as far
/// as the loop can tell.
class Dial final : public mp::IMediaClock {
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

/// An audio graph, as far as a seek is concerned: something with a `seek`.
///
/// That is the whole of what `seek_together` asks of one, which is why it is a
/// template rather than a base class nobody else would implement.
/// A display that takes a millisecond a turn and never runs out.
///
/// `CountedFrames` does not sleep, so a loop driven by it finishes as fast as
/// the CPU can go and is gone before another thread can interact with it. A
/// test about *what happens while the loop is running* needs a loop that is
/// still running.
class SlowFrames final : public mp::IFrameClock {
public:
    bool wait() override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        ticks_ += k_tick_rate / 1000;
        return true;
    }
    [[nodiscard]] std::uint64_t now() const override { return ticks_; }
    [[nodiscard]] std::uint64_t rate() const override { return k_tick_rate; }

private:
    std::uint64_t ticks_ = 0;
};

class FakeAudioGraph {
public:
    explicit FakeAudioGraph(bool movable = true) : movable_(movable) {}

    /// The loop to ask, at the moment the file is asked to move. **Read here
    /// rather than watched from outside**: whether the loop had parked *by
    /// then* is the ordering under test, and a thread polling for it races the
    /// thing it is trying to observe.
    void watches(const mp::DisplayLoop& loop) noexcept { loop_ = &loop; }

    bool seek(std::uint64_t frame)
    {
        asked.push_back(frame);
        parked_when_asked = loop_ != nullptr && loop_->parked();
        return movable_;
    }

    /// What `seek_together` reads to say where the move was aimed in the
    /// picture's seconds: a source counting in 48 kHz frames.
    [[nodiscard]] mp::ClockSpec clock_spec() const noexcept
    {
        mp::ClockSpec spec;
        spec.wire_rate = 48000;
        spec.source_rate = 48000;
        return spec;
    }

    std::vector<std::uint64_t> asked;
    bool parked_when_asked = false;

private:
    bool movable_;
    const mp::DisplayLoop* loop_ = nullptr;
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


TEST_CASE("the refresh is the average over a span, not the shortest gap",
          "[display]")
{
    // **The bias this replaces was measured on real hardware**: the shortest
    // gap came back 0.2 ms under a 16.67 ms refresh, one part in eighty, when
    // what the measurement exists to catch is a crystal's tens of parts in a
    // million. Here the noise is put in on purpose so the answer is known.
    Standing standing;
    Dial dial;

    // 60 Hz exactly, observed through +/- 0.4 ms of noise -- twice the swing
    // the real loop showed, so the old rule would be twice as wrong.
    const std::uint64_t step = k_tick_rate / 60;
    const std::uint64_t swing = k_tick_rate / 2500; // 0.4 ms
    JitteryFrames frames{200, step, swing};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    loop.run();

    const double truth = static_cast<double>(step) / k_tick_rate;
    const double noise = static_cast<double>(swing) / k_tick_rate;
    const mp::DisplayLoop::Stats stats = loop.stats();
    REQUIRE(stats.refresh_span > 190u);

    // **The bound is derived rather than chosen.** After the span average, the
    // only noise left is the two endpoints', and it is divided by the elapsed
    // time between them -- so this is what the arithmetic allows and not a
    // number that happened to pass.
    const double allowed =
        2.0 * noise / (static_cast<double>(stats.refresh_span) * truth);
    const double got = std::abs(stats.refresh_seconds - truth) / truth;
    INFO("measured " << stats.refresh_seconds << " against " << truth << " over "
                     << stats.refresh_span << " refreshes: " << got << " out, allowed "
                     << allowed);
    CHECK(got <= allowed);

    // And what the shortest gap would have said. The phases run +s, 0, -s, 0,
    // so the shortest gap is a whole swing under the truth -- and stays there
    // however long the run is, which is the difference that matters.
    const double old_way = std::abs((truth - noise) - truth) / truth;
    INFO("the shortest gap would be " << old_way << " out");
    CHECK(old_way > 100.0 * got);
}

TEST_CASE("a turn that missed a vertical blank is counted, not discarded",
          "[display]")
{
    // A gap of three refreshes is three refreshes of evidence. Throwing it away
    // would be throwing away the run either side of it as well, because what
    // the average needs is elapsed time and the count that goes with it.
    Standing standing;
    Dial dial;

    const std::uint64_t step = k_tick_rate / 50; // 50 Hz, an exact tick count
    JitteryFrames frames{120, step, k_tick_rate / 5000, 7};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    loop.run();

    const mp::DisplayLoop::Stats stats = loop.stats();
    INFO("measured " << stats.refresh_seconds << " over " << stats.refresh_span);
    CHECK(stats.refresh_seconds ==
          Catch::Approx(static_cast<double>(step) / k_tick_rate).epsilon(3e-4));
    // More refreshes than turns, which is the whole point: the missed blanks
    // are in the count.
    CHECK(stats.refresh_span > stats.turns);
}

TEST_CASE("a run that starts on a starved turn still finds the refresh",
          "[display]")
{
    // **The failure the scale guard exists for.** If the first gap is three
    // refreshes, nothing yet says a refresh is shorter, so it is taken for one
    // and every later gap reads as a third of one. Without the guard the
    // estimate would sit at three times the truth for the whole run.
    Standing standing;
    Dial dial;

    const std::uint64_t step = k_tick_rate / 60;
    // `miss` of 1 makes the *first* turn a three-refresh one, and every one
    // after it, so this also checks that a display seen only at a third of its
    // rate is measured as what it was seen at rather than as nonsense.
    JitteryFrames start_bad{60, step, k_tick_rate / 5000, 1};
    mp::DisplayLoop first{standing.graph, dial, start_bad};
    first.run();
    CHECK(first.stats().refresh_seconds == Catch::Approx(3.0 / 60.0).epsilon(1e-3));

    // And the case that matters: one starved turn at the start, then a normal
    // run. The guard throws away the span built against the wrong scale.
    Standing again;
    Dial dial2;
    JitteryFrames recovers{200, step, k_tick_rate / 5000, 200};
    mp::DisplayLoop second{again.graph, dial2, recovers};
    second.run();
    INFO("measured " << second.stats().refresh_seconds);
    CHECK(second.stats().refresh_seconds == Catch::Approx(1.0 / 60.0).epsilon(1e-3));
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


TEST_CASE("a held loop keeps turning and stops deciding", "[display][avsync]")
{
    Standing standing;
    Dial dial;
    CountedFrames frames{8, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    const std::uint64_t reads_before = dial.reads();
    const std::uint64_t shown_before = standing.graph.stats().shown;

    loop.hold();
    REQUIRE(loop.once(step));
    // **It turned.** The display still has to be waited on and a window still
    // has to stay responsive; what stops is deciding, not the loop.
    CHECK(loop.stats().turns == 2);
    CHECK(loop.parked());
    // And it decided nothing: no clock was read and no frame was shown. The
    // picture already up stays up, which is §8's duplicate and costs nothing.
    CHECK(step.step == mp::VideoGraph::Step::repeated);
    CHECK_FALSE(step.had_clock);
    CHECK(dial.reads() == reads_before);
    CHECK(standing.graph.stats().shown == shown_before);

    loop.release();
    REQUIRE(loop.once(step));
    CHECK_FALSE(loop.parked());
    CHECK(step.had_clock);
}

TEST_CASE("a rewound graph forgets the frame it was holding", "[display][avsync]")
{
    Standing standing;
    Dial dial;
    CountedFrames frames{8, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};

    dial.set(48000, k_tick_rate);
    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    const auto before = standing.graph.stats();
    REQUIRE(before.decoded > 0);

    standing.graph.rewound();

    // **The counters are the run's, not the last seek's.** A report that forgot
    // the frames before a seek would describe the seek rather than the
    // playback.
    const auto after = standing.graph.stats();
    CHECK(after.decoded == before.decoded);
    CHECK(after.shown == before.shown);
    CHECK(after.dropped == before.dropped);
    // And it carries on, which is the part that says the decoder was left
    // usable rather than merely emptied.
    CHECK_FALSE(standing.graph.finished());
    REQUIRE(loop.once(step));
    CHECK(standing.graph.error() == MP_OK);
}

TEST_CASE("seeking moves the loop, the file and the decoder in that order",
          "[display][avsync]")
{
    Standing standing;
    Dial dial;
    CountedFrames frames{8, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    mp::DisplayStep step;
    REQUIRE(loop.once(step));

    FakeAudioGraph audio;
    // The loop is not running on a thread here, so it cannot park itself: the
    // deadline is what stops this from waiting for a turn nobody will take.
    // What is checked is that the file was asked to move exactly once, and
    // that the graph was told about it.
    CHECK(mp::seek_together(audio, standing.graph, loop, 48000 * 42,
                            std::chrono::milliseconds{1}));
    REQUIRE(audio.asked.size() == 1);
    CHECK(audio.asked[0] == 48000 * 42);
    // And the hold was let go, so the next turn decides again.
    CHECK_FALSE(loop.parked());
    REQUIRE(loop.once(step));
    CHECK(step.had_clock);
}

TEST_CASE("a seek waits for the loop to stop deciding before the file moves",
          "[display][avsync]")
{
    Standing standing;
    Dial dial;
    // A clock that takes a millisecond a turn, so the loop is still running
    // when the seek arrives. A loop that had already finished would make the
    // assertion below pass without testing anything.
    SlowFrames frames;
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    std::atomic<bool> stop{false};
    std::thread turning{[&] {
        mp::DisplayStep step;
        while (!stop.load(std::memory_order_acquire) && loop.once(step)) {
        }
    }};

    FakeAudioGraph audio;
    audio.watches(loop);
    // A tiny sleep so the loop is definitely inside its turns rather than not
    // started, which would make the assertion below vacuous.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const std::uint64_t turning_at = loop.stats().turns;
    REQUIRE(turning_at > 0);

    CHECK(mp::seek_together(audio, standing.graph, loop, 4242));
    stop.store(true, std::memory_order_release);
    turning.join();

    REQUIRE(audio.asked.size() == 1);
    // The loop really was turning when the seek arrived, so the wait below is
    // a wait for something rather than a wait for a thread that had stopped.
    CHECK(loop.stats().turns > turning_at);
    // **The ordering this whole call exists for.** Resetting a decoder
    // underneath a thread inside `pump` is how a seek becomes a crash.
    CHECK(audio.parked_when_asked);
}

TEST_CASE("a source that will not seek says so, and the picture is still sound",
          "[display][avsync]")
{
    Standing standing;
    Dial dial;
    CountedFrames frames{8, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);
    mp::DisplayStep step;
    REQUIRE(loop.once(step));

    FakeAudioGraph audio{false};
    CHECK_FALSE(mp::seek_together(audio, standing.graph, loop, 99,
                                  std::chrono::milliseconds{1}));
    // Told anyway: a source that refused may still have been asked, and a
    // decoder holding frames from a half-moved position is worse than one
    // holding none. The cost of being wrong here is one repeated picture.
    CHECK_FALSE(loop.parked());
    REQUIRE(loop.once(step));
    CHECK(standing.graph.error() == MP_OK);
}

TEST_CASE("the video engine's own clock counts, pauses and re-anchors like a device",
          "[display][clock]")
{
    // **What a picture with no sound follows.** `FreeClock` counts a counter
    // and answers `IMediaClock` exactly as an audio graph does: a pause is a
    // count that stops, a seek is an anchor that moves while the count goes
    // on, and a reading is stamped with the counter now even while paused --
    // so a follower extrapolating from it does not walk off on its own.
    CountedFrames counter{1000, 100'000}; // ten milliseconds a step, at ten megahertz
    mp::FreeClock clock{counter, 1000};
    mp::ClockReading reading{};
    CHECK(!clock.read(reading)); // not started: nothing to follow yet

    clock.start();
    counter.wait();
    counter.wait();
    counter.wait();
    REQUIRE(clock.read(reading));
    CHECK(reading.device_frames == 30);
    CHECK(reading.ticks == counter.now());
    CHECK(clock.position() == 30);

    clock.pause();
    counter.wait();
    counter.wait();
    REQUIRE(clock.read(reading));
    CHECK(reading.device_frames == 30);
    CHECK(reading.ticks == counter.now());
    CHECK(clock.paused());
    clock.resume();
    counter.wait();
    CHECK(clock.position() == 40);

    clock.seek(5000);
    const mp::ClockSpec spec = clock.spec();
    CHECK(spec.origin_device_frame == 40);
    CHECK(spec.origin_source_frame == 5000);
    counter.wait();
    CHECK(clock.position() == 5010);
    REQUIRE(clock.read(reading));
    CHECK(reading.device_frames == 50); // the count went on through the seek

    // Followed through the same arithmetic the audio device is followed with.
    mp::AvClock followed;
    mp::ClockSpec filled = clock.spec();
    filled.tick_rate = counter.rate();
    followed.configure(filled);
    followed.observe(reading);
    CHECK(followed.audible_frames(counter.now()) == Catch::Approx(5010.0));
}

TEST_CASE("a job posted to the loop runs on the loop's own thread, between turns",
          "[display]")
{
    // **A setting is a message to the thread that owns the presenter.** The
    // job is queued from this thread and run by whichever thread takes the
    // next turn, before that turn decides anything.
    Standing standing;
    Dial dial;
    CountedFrames frames{6, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    std::thread::id ran_on{};
    std::uint64_t turns_when_run = 99;
    std::future<bool> done = loop.post([&] {
        ran_on = std::this_thread::get_id();
        turns_when_run = loop.stats().turns;
    });
    CHECK(done.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

    std::thread::id turner_id{};
    bool turned = false;
    std::thread turner{[&] {
        turner_id = std::this_thread::get_id();
        mp::DisplayStep step;
        turned = loop.once(step);
    }};
    turner.join();
    REQUIRE(turned);
    REQUIRE(done.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    CHECK(done.get());
    CHECK(ran_on == turner_id);
    CHECK(ran_on != std::this_thread::get_id());
    // Before the turn was counted, which is before anything was decided.
    CHECK(turns_when_run == 0u);
}

TEST_CASE("a job posted to a loop that has stopped is answered false, and never run",
          "[display]")
{
    Standing standing;
    Dial dial;
    CountedFrames frames{1, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    mp::DisplayStep step;
    REQUIRE(loop.once(step));
    // The clock has run out: the next turn says stop, and a job queued before
    // it is answered false rather than run against a loop that is ending.
    bool ran = false;
    std::future<bool> before = loop.post([&] { ran = true; });
    REQUIRE_FALSE(loop.once(step));
    REQUIRE(before.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    CHECK_FALSE(before.get());
    CHECK_FALSE(ran);
    // And one posted after the end is answered at once.
    std::future<bool> after = loop.post([&] { ran = true; });
    REQUIRE(after.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    CHECK_FALSE(after.get());
    CHECK_FALSE(ran);
}

TEST_CASE("a hold is answered for its own serial, and counted", "[display][avsync]")
{
    // **The two instructions.** With one flag, a holder that arrived between
    // the loop reading the flag and writing the answer read the *previous*
    // hold's acknowledgement as its own and moved the file under a turn that
    // was deciding. A serial is the answer to one request and no other, and a
    // count keeps two holders from letting go of each other's hold.
    Standing standing;
    Dial dial;
    CountedFrames frames{12, k_tick_rate};
    mp::DisplayLoop loop{standing.graph, dial, frames};
    dial.set(48000, k_tick_rate);

    mp::DisplayStep step;
    REQUIRE(loop.once(step));

    const std::uint64_t first = loop.hold();
    CHECK_FALSE(loop.parked(first));
    REQUIRE(loop.once(step));
    CHECK(loop.parked(first));
    CHECK(loop.parked());
    loop.release();

    // Not yet acknowledged: the loop has not turned since this hold, whatever
    // it said about the last one.
    const std::uint64_t second = loop.hold();
    CHECK_FALSE(loop.parked(second));
    CHECK_FALSE(loop.parked());
    REQUIRE(loop.once(step));
    CHECK(loop.parked(second));

    // A second holder, and the first letting go, leave the loop parked.
    const std::uint64_t third = loop.hold();
    REQUIRE(loop.once(step));
    CHECK(loop.parked(third));
    loop.release();
    REQUIRE(loop.once(step));
    CHECK(loop.parked());
    CHECK(step.step == mp::VideoGraph::Step::repeated);
    loop.release();
    REQUIRE(loop.once(step));
    CHECK_FALSE(loop.parked());
    CHECK(step.had_clock);
}

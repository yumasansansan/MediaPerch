// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loop that calls `VideoGraph::pump`, and the two clocks it needs.
//
// **The policy is here and the clocks are not**, which is the same split §8
// already makes. Waiting for a display and reading a performance counter are
// platform; deciding what to do each time round is not, and it is the part
// worth testing. So a fake frame clock that returns immediately five hundred
// times, and a fake clock to follow that runs fast or stops, produce the same
// decisions here that a real display and a real sound card would.
//
// **Two clocks, and they are not the same one.** The clock being followed says
// where the presentation is -- the audio device when there is sound, the video
// engine's own `FreeClock` when there is not -- and the display says when a
// picture may be drawn. Neither can be derived from the other: a 60 Hz display
// and a 24 fps film share no factor, and an audio device's crystal is not the
// display's. Which clock is followed is the caller's policy; the loop follows
// whatever `IMediaClock` it is handed, and the two cases are one code path.

#include "mediaperch/avsync.hpp"
#include "mediaperch/clock.hpp"
#include "mediaperch/video.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace mp {

/// When a frame may be drawn, and what time it is.
///
/// One object for both because they are one clock: the tick a frame is drawn
/// at is the tick the audio clock has to be read against, and taking them from
/// two sources would put a scheduling delay between them.
class IFrameClock : public ITicks {
public:
    IFrameClock() = default;
    IFrameClock(const IFrameClock&) = delete;
    IFrameClock& operator=(const IFrameClock&) = delete;
    IFrameClock(IFrameClock&&) = delete;
    IFrameClock& operator=(IFrameClock&&) = delete;
    virtual ~IFrameClock() = default;

    /// Blocks until the display will take another frame. False when it cannot
    /// say -- a display that went away, or a loop that should stop.
    virtual bool wait() = 0;
    /// Makes `wait` answer false, now and from then on.
    ///
    /// **A loop is stopped from outside it.** `wait` blocks for a whole
    /// refresh, and whoever owns the loop needs a way to say stop that does
    /// not take sixteen milliseconds to be heard. The default does nothing,
    /// which is right for a clock that counts: it stops when its count runs
    /// out, and there is nothing for a canceller to do.
    virtual void cancel() noexcept {}
    /// What the display says it refreshes at, in seconds, or zero when it did
    /// not say. A starting point only: `DisplayLoop` measures the real one,
    /// because a display that calls itself 60 Hz is usually 59.94.
    [[nodiscard]] virtual double nominal_interval() const { return 0.0; }
    // `now` and `rate` are `ITicks`'s: the counter a frame is stamped with is
    // the counter the readings it is decided against are stamped with.
};

/// One turn of a display loop.
struct DisplayStep {
    /// What the graph did.
    VideoGraph::Step step = VideoGraph::Step::repeated;
    /// Whether there was a master clock to decide against.
    bool had_clock = false;
};

/// Wait for the display, read the audio clock, pump the video graph.
///
/// **It holds no thread of its own.** `once` is one turn and `run` is the loop;
/// which thread they happen on is the head's business, because on Windows the
/// window that is being drawn into owns a message queue and the thread that
/// pumps it is not a detail this file can decide.
class DisplayLoop final {
public:
    /// `origin_seconds` is **where this track began on the clock being
    /// followed**, and is not zero for anything but the first track of a run.
    ///
    /// §8 makes the audio device the master clock, and a gapless queue is one
    /// stream to that device: the position it reports counts straight through
    /// every track boundary, because not noticing one is what gapless *is*. A
    /// picture's frames do not -- each file's timestamps start at zero -- so
    /// the two are in different coordinates and only the queue knows the offset
    /// between them. Without it, every frame of the second track is late by
    /// however long the first one was, and the loop drops all of them: measured
    /// as `decoded 24, dropped 24, shown 0` on the twentieth track of a
    /// one-second file.
    DisplayLoop(VideoGraph& graph, IMediaClock& follow, IFrameClock& frames,
                double origin_seconds = 0.0) noexcept;

    DisplayLoop(const DisplayLoop&) = delete;
    DisplayLoop& operator=(const DisplayLoop&) = delete;
    DisplayLoop(DisplayLoop&&) = delete;
    DisplayLoop& operator=(DisplayLoop&&) = delete;

    /// One turn: wait, read, pump. `false` when the frame clock said to stop or
    /// the stream ended.
    bool once(DisplayStep& out);

    /// Turns until the stream ends or the frame clock stops. Returns the number
    /// of turns, which for a file is roughly its length times the refresh rate.
    std::uint64_t run();

    // --- holding still while somebody moves the file -------------------------
    //
    // **The same shape the audio graph's render thread already has**, for the
    // same reason: a seek moves the one position two consumers share (§4), and
    // a turn taken while that happens is a turn deciding about a frame from a
    // place nobody is at any more. The loop keeps turning -- the display still
    // has to be waited on and a window still has to stay responsive -- and only
    // stops deciding.

    /// Stop deciding. The picture already up stays up, which is the duplicate
    /// §8 describes and costs nothing to perform.
    ///
    /// **Counted, and numbered.** Two holders at once -- a seek on the engine
    /// thread and a setting on an IPC thread -- must each keep the loop parked
    /// until *both* have let go, so a hold is a count. And the number handed
    /// back is this hold's serial, which `parked(serial)` answers for, so that
    /// a holder cannot take the acknowledgement of somebody's earlier hold for
    /// its own: a single flag let that happen in the two instructions between
    /// the loop reading it and writing the answer.
    std::uint64_t hold() noexcept;
    /// Decide again, once every holder has.
    void release() noexcept;
    /// Whether a turn has been taken under the hold. **The answer a mover
    /// waits for**: `hold` is a request and this is the acknowledgement, and
    /// resetting a decoder underneath a thread that is inside `pump` is how a
    /// seek becomes a crash rather than a seek. Without a serial: whether the
    /// loop is parked for the latest hold there is.
    [[nodiscard]] bool parked() const noexcept;
    [[nodiscard]] bool parked(std::uint64_t serial) const noexcept;

    // --- work that belongs to the loop's own thread ---------------------------
    //
    // **A setting is a message to the thread that owns the presenter.** Once
    // the loop has started it is the one thread inside the presenter's
    // graphics context, and a presenter's `set` or `describe` from any other
    // thread -- an IPC thread carrying a shell's message -- was two threads in
    // one context: a resize that reset the target's views under a draw was a
    // use-after-free with a five-hundred-millisecond spin in front of it, and
    // "Set" was the button that took the engine down. So a job is posted, the
    // loop runs it between two frames on its own thread, and the caller waits
    // on the future with a deadline -- and when the loop does not come round
    // it is told so, never served concurrently.

    using Job = std::function<void()>;
    /// Queue `job` for the loop's next turn. The future is `true` once it has
    /// run and `false` if the loop ended first; a job posted after the end is
    /// answered `false` at once rather than left hanging.
    [[nodiscard]] std::future<bool> post(Job job);
    /// Answer every queued job `false`, and every later one too. What the loop
    /// does when a turn says stop, and what `VideoPath::stop` does after the
    /// join, so that nobody waits on a loop that is gone.
    void close_posts() noexcept;

    /// Where this track began on the clock being followed. See the constructor.
    [[nodiscard]] double origin_seconds() const noexcept { return origin_seconds_; }

    struct Stats {
        std::uint64_t turns = 0;
        /// Turns where the device had no clock to read. **Not an error and not
        /// zero at the start**: a graph that has not started has nothing
        /// playing, and a picture drawn against a clock that is not running
        /// would be a picture drawn against a guess.
        std::uint64_t without_clock = 0;
        /// Turns where the anchor moved -- a start, or a seek.
        std::uint64_t reanchored = 0;
        /// The display's refresh, as measured from the turns themselves. Zero
        /// until two turns have happened.
        ///
        /// **Measured rather than asked**, because a mode that says 60 Hz is
        /// 59.94 and the difference is a frame every seventeen minutes -- the
        /// same rounding §9.9 refuses for a container's frame rate.
        ///
        /// **Elapsed time over refreshes counted, and this used to be the
        /// shortest gap.** The argument for the shortest was that a gap can
        /// only be *lengthened* by a turn that was late, which is true of
        /// outliers and false of jitter: the minimum of a symmetrically noisy
        /// sample sits low by roughly the noise, and it stays there however
        /// long the run is. Measured against the modes this tree built, the
        /// shortest gap came back 0.2 ms under a 16.67 ms refresh and 0.2 ms
        /// under a 20.85 ms one -- one part in eighty, when the deviation the
        /// measurement exists to catch is a crystal's tens of parts in a
        /// million. It was less accurate than believing the mode's own label.
        ///
        /// An average over a span has no such bias. Timestamp noise enters only
        /// at the two ends of the span and is divided by the refreshes between
        /// them, so the error shrinks as the run goes on rather than settling.
        /// What the shortest gap is still for is the *scale*: a gap is worth
        /// one refresh or two or three, and something has to say which.
        double refresh_seconds = 0.0;
        /// How many refresh intervals `refresh_seconds` is averaged over. The
        /// estimate's error is about one timestamp's jitter divided by this, so
        /// it is the number that says how much to believe it.
        ///
        /// An integer because it counts something. The seconds beside it are a
        /// measurement and are a double for the same reason.
        std::uint64_t refresh_span = 0;
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }
    [[nodiscard]] const AvClock& clock() const noexcept { return clock_; }

private:
    /// Re-reads the graph's fixed facts and notices when the anchor moved,
    /// which is what a seek does. Cheap: two atomic loads.
    void refresh_spec();
    /// Learns the display's refresh from the gaps between turns, and tells the
    /// pacer how far ahead to decide.
    void learn_refresh(std::uint64_t ticks);
    /// The interval to pace by now: the span average once there is one, and the
    /// shortest gap until then.
    [[nodiscard]] double interval_now() const noexcept;

    bool turn(DisplayStep& out);
    void drain_posts();

    std::atomic<std::uint64_t> holders_{0};
    std::atomic<std::uint64_t> hold_serial_{0};
    std::atomic<std::uint64_t> parked_serial_{0};

    std::mutex posts_mutex_;
    std::deque<std::pair<Job, std::promise<bool>>> posts_;
    bool posts_closed_ = false;

    VideoGraph* graph_;
    IMediaClock* follow_;
    IFrameClock* frames_;
    double origin_seconds_ = 0.0;
    AvClock clock_;
    ClockSpec spec_{};
    bool configured_ = false;
    std::uint64_t last_tick_ = 0;
    bool have_last_tick_ = false;
    /// The shortest gap seen, which is the scale rather than the answer: it
    /// decides whether a gap was one refresh or three.
    double shortest_gap_ = 0.0;
    /// Seconds of run, and refreshes in them, over the gaps that were a whole
    /// number of refreshes long.
    double span_seconds_ = 0.0;
    std::uint64_t span_refreshes_ = 0;
    Stats stats_{};
};


/// **A file with a picture in it, seeked.**
///
/// §4 gives a file one position and the router owns it, so there is no such
/// thing as seeking the audio and seeking the video: there is one move, and
/// three things have to agree about it. This is that move, and the order is the
/// whole of it.
///
/// 1. **Hold the display loop**, and wait until it says it has stopped
///    deciding. Resetting a decoder underneath a thread inside `pump` is how a
///    seek becomes a crash.
/// 2. **Seek the audio graph.** It parks its own render thread, resets its
///    ring, and asks its source -- which is a router feed, so this is what
///    moves the file. `PacketRouter::seek` clears *every* queue, the video's
///    included, because the packets waiting for the other consumer came from
///    where the file used to be. The call returns once the audio has actually
///    moved, so there is nothing to poll.
/// 3. **Tell the video graph it was rewound**, now that its queue is empty and
///    the position is elsewhere.
/// 4. **Release the loop.** It re-reads the audio graph's anchor on its next
///    turn -- `origin_device_frame` and `origin_source_frame` moved during step
///    2 -- and `AvClock` re-anchors without the device ever stopping.
///
/// A template because the two audio graphs are two types with no common base,
/// and `Player` already answers that question this way. §15's rule is that an
/// interface waits for the second implementation; a base class invented for one
/// method here would be the interface arriving early.
///
/// False when the source will not seek. `deadline` is how long to wait for the
/// loop to park before moving anyway -- a loop that is not running has no turn
/// to take, and refusing to seek because nobody is drawing would be refusing
/// the seek for the wrong reason.
template <typename AudioGraph>
bool seek_together(AudioGraph& audio, VideoGraph& video, DisplayLoop& loop,
                   std::uint64_t frame,
                   std::chrono::milliseconds deadline = std::chrono::milliseconds{500})
{
    const std::uint64_t held = loop.hold();
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop.parked(held) && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    const bool moved = audio.seek(frame);
    // **Even when it did not move.** A source that refused the seek may still
    // have been asked, and a decoder holding frames from a position that was
    // half-moved is worse than one holding none. The cost of being wrong here
    // is one repeated picture. And where the move was aimed, in this track's
    // own seconds, so what the decoder emits on the way there is counted as
    // the container's pre-roll rather than as frames dropped.
    const std::uint32_t rate = audio.clock_spec().source_rate;
    video.rewound(moved && rate != 0
                      ? static_cast<double>(frame) / static_cast<double>(rate) -
                            loop.origin_seconds()
                      : -1.0);

    loop.release();
    return moved;
}

} // namespace mp

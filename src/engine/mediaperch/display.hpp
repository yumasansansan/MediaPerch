// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loop that calls `VideoGraph::pump`, and the two clocks it needs.
//
// **The policy is here and the clocks are not**, which is the same split §8
// already makes. Waiting for a display and reading a performance counter are
// platform; deciding what to do each time round is not, and it is the part
// worth testing. So a fake frame clock that returns immediately five hundred
// times, and a fake audio clock that runs fast or stops, produce the same
// decisions here that a real display and a real sound card would.
//
// **Two clocks, and they are not the same one.** The audio device says where
// the sound is -- §8's master -- and the display says when a picture may be
// drawn. Neither can be derived from the other: a 60 Hz display and a 24 fps
// film share no factor, and an audio device's crystal is not the display's.

#include "mediaperch/avsync.hpp"
#include "mediaperch/video.hpp"

#include <cstdint>

namespace mp {

/// When a frame may be drawn, and what time it is.
///
/// One object for both because they are one clock: the tick a frame is drawn
/// at is the tick the audio clock has to be read against, and taking them from
/// two sources would put a scheduling delay between them.
class IFrameClock {
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
    /// What the display says it refreshes at, in seconds, or zero when it did
    /// not say. A starting point only: `DisplayLoop` measures the real one,
    /// because a display that calls itself 60 Hz is usually 59.94.
    [[nodiscard]] virtual double nominal_interval() const { return 0.0; }
    /// The counter `ClockReading::ticks` is stamped with.
    [[nodiscard]] virtual std::uint64_t now() const = 0;
    /// Its ticks per second.
    [[nodiscard]] virtual std::uint64_t rate() const = 0;
};

/// Where §8's master clock comes from.
///
/// The graphs answer both of these and are the only things that know all of
/// it; a test answers them with numbers it chose.
class IAudioClockSource {
public:
    IAudioClockSource() = default;
    IAudioClockSource(const IAudioClockSource&) = delete;
    IAudioClockSource& operator=(const IAudioClockSource&) = delete;
    IAudioClockSource(IAudioClockSource&&) = delete;
    IAudioClockSource& operator=(IAudioClockSource&&) = delete;
    virtual ~IAudioClockSource() = default;

    [[nodiscard]] virtual ClockSpec spec() const = 0;
    /// False when the device has no clock, which is a sink module that did not
    /// implement `get_position`.
    virtual bool read(ClockReading& out) = 0;
};

/// Either audio graph, as a clock. Both answer the same two calls.
template <class Graph>
class GraphClock final : public IAudioClockSource {
public:
    explicit GraphClock(Graph& graph) noexcept : graph_(&graph) {}

    [[nodiscard]] ClockSpec spec() const override { return graph_->clock_spec(); }
    bool read(ClockReading& out) override { return graph_->read_clock(out); }

private:
    Graph* graph_;
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
    DisplayLoop(VideoGraph& graph, IAudioClockSource& audio, IFrameClock& frames) noexcept;

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
        /// same rounding §9.9 refuses for a container's frame rate. Taken as
        /// the shortest gap seen rather than the average: a gap can only be
        /// lengthened by a turn that was late, so the shortest is the one that
        /// was not.
        double refresh_seconds = 0.0;
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

    VideoGraph* graph_;
    IAudioClockSource* audio_;
    IFrameClock* frames_;
    AvClock clock_;
    ClockSpec spec_{};
    bool configured_ = false;
    std::uint64_t last_tick_ = 0;
    bool have_last_tick_ = false;
    Stats stats_{};
};

} // namespace mp

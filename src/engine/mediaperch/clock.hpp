// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A clock, in the words the two engines hand one to each other in.
//
// **Each engine keeps its own clock, and this is the shape they agree on.**
// The audio engine's clock is the device: what it has played, stamped with
// when it said so. The video engine's is the display, which says when a frame
// may be drawn, and `FreeClock` below for the timeline when there is nothing
// else to follow. A player running both puts one in front of the other -- the
// picture follows the sound, because the sound is bit-exact and cannot be
// moved -- and that coupling is `AvClock`'s, which is the video engine's.
// Nothing here belongs to either engine, which is what lets each be built and
// linked without the other: the audio library answers this interface and never
// includes a video header, and the video library follows it and never includes
// an audio one. tests/audio_alone.cpp and tests/video_alone.cpp are the proof,
// one link line each, and cmake/CorePurity.cmake reads the includes.
//
// **No clock is read here.** These are the words; the counters are the heads'.
// `FreeClock` counts whatever `ITicks` it is given, which on Windows is the
// performance counter behind a frame clock and in a test is a number.

#include <cstdint>
#include <mutex>

namespace mp {

/// One reading of a clock: what it has played, and the counter tick it said so
/// at. For a device both come from `MpSinkVtbl::get_position` in one call, and
/// the pair is what makes extrapolation possible: a reading on its own is
/// already out of date by the time anybody looks at it.
struct ClockReading {
    /// Device frames played since the stream started, in the *wire* format's
    /// frames -- which is what the device counts and not always what the file
    /// counts.
    std::uint64_t device_frames = 0;
    /// The performance counter when the device said so.
    std::uint64_t ticks = 0;
};

/// The fixed facts a clock's owner knows and a follower needs.
///
/// An audio graph is the one thing that knows all five: it owns the sink, it
/// converted the source to the wire format, it built the DSP chain, and it
/// remembers where the run began. See `PassthroughGraph::clock_spec`. A
/// `FreeClock` answers the same five about itself.
struct ClockSpec {
    /// What the device counts in.
    std::uint32_t wire_rate = 0;
    /// What the file counts in. The same number unless something in the chain
    /// changed the rate, and then it is not, so the ratio is applied rather
    /// than assumed away.
    std::uint32_t source_rate = 0;
    /// Ticks per second of whatever counter `ClockReading::ticks` came from.
    std::uint64_t tick_rate = 0;
    /// The DSP chain's, in wire frames. **What is audible is this far behind
    /// what the device position says**: the frames coming out of the endpoint
    /// went through the chain, and a linear-phase stage delayed them. Zero on
    /// Path A, which has no chain in it by construction.
    std::uint32_t latency_frames = 0;
    /// The anchor. At device frame `origin_device_frame`, the source was at
    /// `origin_source_frame`.
    ///
    /// **A seek moves it and does not stop the device.** The audio already
    /// committed before a seek still plays, so the frame seeked to becomes
    /// audible when the device reaches what had been *written* at that moment,
    /// not what it had played. The graphs have kept exactly those two numbers
    /// since gapless was written; nothing had read them against the device.
    std::uint64_t origin_device_frame = 0;
    std::uint64_t origin_source_frame = 0;
};


/// A counter: what `ClockReading::ticks` is stamped with. A display's frame
/// clock is one, and so is anything a head reads the performance counter
/// through; a test's is an integer it adds to.
class ITicks {
public:
    ITicks() = default;
    ITicks(const ITicks&) = delete;
    ITicks& operator=(const ITicks&) = delete;
    ITicks(ITicks&&) = delete;
    ITicks& operator=(ITicks&&) = delete;
    virtual ~ITicks() = default;

    /// The counter now.
    [[nodiscard]] virtual std::uint64_t now() const = 0;
    /// Its ticks per second.
    [[nodiscard]] virtual std::uint64_t rate() const = 0;
};

/// A clock somebody can follow: where the presentation is, and the facts a
/// follower needs to extrapolate it between readings.
///
/// The audio graphs answer this from the device and are the only things that
/// know all of it; `FreeClock` answers it from a counter; a test answers it
/// with numbers it chose.
class IMediaClock {
public:
    IMediaClock() = default;
    IMediaClock(const IMediaClock&) = delete;
    IMediaClock& operator=(const IMediaClock&) = delete;
    IMediaClock(IMediaClock&&) = delete;
    IMediaClock& operator=(IMediaClock&&) = delete;
    virtual ~IMediaClock() = default;

    [[nodiscard]] virtual ClockSpec spec() const = 0;
    /// False when there is no clock to read -- a device whose sink module did
    /// not implement `get_position`, or a `FreeClock` nobody has started.
    virtual bool read(ClockReading& out) = 0;
};

/// Either audio graph, as a clock. Both answer the same two calls, and so
/// would anything else with a `clock_spec` and a `read_clock`.
template <class Graph>
class GraphClock final : public IMediaClock {
public:
    explicit GraphClock(Graph& graph) noexcept : graph_(&graph) {}

    [[nodiscard]] ClockSpec spec() const override { return graph_->clock_spec(); }
    bool read(ClockReading& out) override { return graph_->read_clock(out); }

private:
    Graph* graph_;
};

/// **The video engine's own timeline**, for a picture with nothing to follow.
///
/// Counts a counter and reports it as a device would, at `rate`: pausing
/// freezes the count, and a seek moves the anchor in `spec()` and does not
/// stop the count, which is exactly what a device does -- so a follower cannot
/// tell the two apart, and `DisplayLoop` has one code path rather than two.
///
/// What it costs is that the counter and the display are not the same
/// crystal, so a long run drifts against the display by the difference
/// between them. It does not drift against anything a person can hear,
/// because there is nothing to hear. Locked, because the loop reads it on its
/// thread and a transport moves it from another; nothing real-time is on
/// either side.
class FreeClock final : public IMediaClock {
public:
    FreeClock(const ITicks& counter, std::uint32_t rate) noexcept;

    [[nodiscard]] ClockSpec spec() const override;
    bool read(ClockReading& out) override;

    /// Begins counting, from wherever the last `seek` put the timeline or
    /// from zero. Before this, `read` answers false, which is what keeps a
    /// picture from being drawn against a clock that has not begun.
    void start();
    void pause();
    void resume();
    [[nodiscard]] bool paused() const;
    /// Moves the timeline to `frame`, at `rate`, without stopping the count.
    void seek(std::uint64_t frame);
    /// Where the timeline is now, in frames at `rate`.
    [[nodiscard]] std::uint64_t position() const;
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }

private:
    /// What the count says now, in frames at `rate`; frozen while paused.
    /// The lock is held by the caller.
    [[nodiscard]] std::uint64_t counted() const noexcept;

    const ITicks* counter_;
    std::uint32_t rate_;
    mutable std::mutex lock_;
    ClockSpec spec_{};
    bool started_ = false;
    bool paused_ = false;
    /// The counter when the count began, the ticks spent paused since, which
    /// the count leaves out, and when the current pause began.
    std::uint64_t began_ = 0;
    std::uint64_t held_ = 0;
    std::uint64_t pause_began_ = 0;
};

} // namespace mp

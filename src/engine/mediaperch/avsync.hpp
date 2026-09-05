// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// §8: the audio device is the master clock, and video is what moves.
//
// **The consequence is the opposite of the usual answer, and it is the reason
// this file exists at all.** In exclusive passthrough there is no resampler, so
// audio cannot be rate-matched to video: Path A hands the device the file's own
// bytes and any adjustment would be a conversion this player did not announce.
// So video drops or duplicates frames against the audio clock, always, and
// nothing here can move audio in the other direction -- there is no method for
// it, which is the strongest way to say it.
//
// **No clock is read here.** `AvClock` is given a device reading and a
// performance-counter tick and does arithmetic on them; it never calls
// QueryPerformanceCounter, never touches a sink, and knows nothing about
// Windows. That is what makes §8 testable without a sound card -- a test hands
// it a clock that runs fast, or slow, or stops, and checks what the video side
// was told to do -- and what makes it the same file on the Linux head, where
// the ticks come from CLOCK_MONOTONIC instead.
//
// **Nothing accumulates.** Every answer is computed from the latest reading, so
// there is no running total to drift: an error in one reading is gone by the
// next one rather than added to a sum. That is also the argument for doing the
// comparison in `double` at all. §9.9 says converting a timebase to nanoseconds
// is how a player drifts, and it is right -- but that is about *rounding every
// timestamp into a unit that cannot hold it*, which nothing here does: the
// timestamps stay in the container's own integer ticks, and one division
// happens at the point of comparison, on numbers whose difference is a few
// milliseconds.

#include <cstdint>

namespace mp {

/// One reading of the device's clock: what it says it has played, and the
/// performance-counter tick it said so at.
///
/// Both come from `MpSinkVtbl::get_position` in one call, and the pair is what
/// makes extrapolation possible: a reading on its own is already out of date by
/// the time anybody looks at it.
struct ClockReading {
    /// Device frames played since the stream started, in the *wire* format's
    /// frames -- which is what the device counts and not always what the file
    /// counts.
    std::uint64_t device_frames = 0;
    /// The performance counter when the device said so.
    std::uint64_t ticks = 0;
};

/// The fixed facts a graph knows and a clock needs.
///
/// A graph is the one thing that knows all five: it owns the sink, it converted
/// the source to the wire format, it built the DSP chain, and it remembers
/// where the run began. See `PassthroughGraph::clock_spec`.
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

/// Where the audio leaving the speaker is, right now.
class AvClock {
public:
    AvClock() = default;

    /// Sets the fixed facts and forgets any reading. Call it again after a seek
    /// or a rebuild; the anchor is in the spec.
    void configure(const ClockSpec& spec) noexcept;

    /// A fresh reading. Cheap, and worth doing often: everything below
    /// extrapolates from the last one, and extrapolation is a guess about a
    /// crystal nobody here owns.
    void observe(const ClockReading& reading) noexcept;

    /// Whether a reading has arrived. Before one has, there is no master clock
    /// and nothing may be synchronised to it -- **not even approximately**,
    /// which is why this is a question rather than a plausible zero.
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    [[nodiscard]] const ClockSpec& spec() const noexcept { return spec_; }

    /// Which source frame is leaving the speaker at `ticks`.
    ///
    /// Never before the anchor: at the moment a run or a seek began, nothing
    /// after it had come out yet, so the answer is clamped there rather than
    /// running backwards into audio that belongs to a position nobody is at.
    [[nodiscard]] double audible_frames(std::uint64_t ticks) const noexcept;

    /// The same answer in the stream's own seconds, which is the unit a video
    /// timestamp turns into.
    [[nodiscard]] double audible_seconds(std::uint64_t ticks) const noexcept;

private:
    ClockSpec spec_{};
    ClockReading reading_{};
    bool ready_ = false;
};

/// Seconds into the stream, from a timestamp in the container's own ticks.
///
/// One division and no rounding: `MpVideoInfo::timescale` is ticks per second
/// because that is the form both containers store, and 24000/1001 is not a
/// decimal. Zero timescale answers zero, because a stream that did not say
/// cannot be placed on a timeline.
[[nodiscard]] double stream_seconds(std::uint64_t ticks, std::uint32_t timescale) noexcept;

/// What to do with one decoded video frame.
enum class FrameFate {
    /// Present it: its time has come.
    show,
    /// Not yet. The caller comes back with the *same* frame later, and the
    /// picture already on screen stays up -- **which is the duplicate**. There
    /// is no separate instruction for duplicating a frame because there is no
    /// separate act: a display that is not given a new frame shows the old one.
    repeat,
    /// Too late to be worth showing. Take the next one.
    drop,
};

/// §8's rule, applied one frame at a time.
///
/// Two thresholds and no more. A frame is shown once the clock has reached its
/// timestamp, and dropped once the clock has passed it by more than one frame
/// interval -- because at that point the *next* frame is already due, and
/// showing this one only makes that one later too.
///
/// **The interval comes from the container when it says so.** `fps_num/fps_den`
/// is a ratio for the reason MpVideoInfo gives: rounding 24000/1001 is how a
/// player drifts a frame every seventeen minutes. When the container does not
/// state a rate -- normal for one that timestamps every frame instead -- it is
/// measured from consecutive timestamps, and until two have been seen nothing
/// is dropped at all. That is the safe direction: a late frame shown is a
/// blemish, and a frame dropped because the rate was guessed is gone.
class VideoPacer {
public:
    VideoPacer() = default;

    /// `timescale`, `fps_num` and `fps_den` are `MpVideoInfo`'s, verbatim.
    void configure(std::uint32_t timescale, std::uint32_t fps_num,
                   std::uint32_t fps_den) noexcept;

    /// How long after this decision the frame will actually be on screen.
    ///
    /// **A decision made at a vertical blank shows a frame at the next one.**
    /// `present` hands the frame to the swap chain and the compositor puts it
    /// up at the following blank, so a loop that asked "is it due now" was
    /// really asking the wrong question: what matters is whether the frame is
    /// due at the moment it will appear. Answering the wrong one costs up to a
    /// whole refresh, every time, and it was measured -- 16.6 ms against a
    /// 16.7 ms refresh, which is the floor of a loop deciding at blank
    /// boundaries and asking about the blank it is standing on.
    ///
    /// With the interval given, "up to one refresh late" becomes "half a
    /// refresh either side", because the frame chosen is the one whose time
    /// the presentation instant is nearest to -- `decide` rounds to the
    /// nearest presentation rather than flooring to the next one, and half a
    /// lead is what "nearest" means when presentations are a lead apart.
    ///
    /// Zero is the default and means "now", which is right for a caller with
    /// no display -- a test, or anything measuring the arithmetic rather than
    /// a picture.
    void set_lead_seconds(double seconds) noexcept { lead_ = seconds; }

    /// How far the video timeline is from the audio one, in seconds.
    ///
    /// **Not hypothetical.** §9.9 found sixty milliseconds of it in this tree's
    /// own fixture: a container states a per-track edit, the engine applies the
    /// audio track's before the source counts a frame, and the video timestamps
    /// stay container-relative. Whoever knows both edits subtracts them once,
    /// here, rather than each side folding it in differently.
    void set_skew_seconds(double seconds) noexcept { skew_ = seconds; }

    /// Forgets the measured interval and the last timestamp. After a seek the
    /// next timestamp is not the one after the last.
    void reset() noexcept;

    struct Decision {
        FrameFate fate = FrameFate::show;
        /// Positive when the frame is early, negative when it is late,
        /// **measured against the moment it will be on screen** -- which is
        /// `lead_seconds` after the decision. With a lead set it straddles
        /// zero; with none it is never positive for a frame that was shown.
        double error_seconds = 0.0;
    };

    /// What to do with a frame timestamped `pts_ticks`, when the audio being
    /// heard is `audible_seconds` into the stream.
    ///
    /// **Asking twice about the same frame is safe**, which is what makes
    /// `repeat` usable: nothing is recorded and no interval is learned until
    /// the answer is one that consumes the frame. A caller that polls a frame
    /// which is not due yet gets the same answer and moves no state.
    [[nodiscard]] Decision decide(std::uint64_t pts_ticks, double audible_seconds) noexcept;

    struct Stats {
        std::uint64_t shown = 0;
        std::uint64_t dropped = 0;
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }

    /// One frame's worth of time, or zero when it is not known yet.
    [[nodiscard]] double interval_seconds() const noexcept;

private:
    std::uint32_t timescale_ = 0;
    std::uint32_t fps_num_ = 0;
    std::uint32_t fps_den_ = 0;
    double skew_ = 0.0;
    double lead_ = 0.0;
    double measured_ = 0.0;
    std::uint64_t last_pts_ = 0;
    bool have_last_ = false;
    Stats stats_{};
};

} // namespace mp

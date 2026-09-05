// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/avsync.hpp"

namespace mp {

namespace {

/// A tick difference that keeps its sign. The two values are unsigned and the
/// question is asked about times on both sides of the reading -- a caller
/// deciding about a frame may well be a moment behind the last reading -- so
/// subtracting them the obvious way wraps instead of going negative.
double seconds_between(std::uint64_t from, std::uint64_t to, std::uint64_t rate) noexcept
{
    if (rate == 0) {
        return 0.0;
    }
    const bool forward = to >= from;
    const std::uint64_t magnitude = forward ? to - from : from - to;
    const double seconds = static_cast<double>(magnitude) / static_cast<double>(rate);
    return forward ? seconds : -seconds;
}

} // namespace

void AvClock::configure(const ClockSpec& spec) noexcept
{
    spec_ = spec;
    reading_ = ClockReading{};
    ready_ = false;
}

void AvClock::observe(const ClockReading& reading) noexcept
{
    reading_ = reading;
    ready_ = true;
}

double AvClock::audible_frames(std::uint64_t ticks) const noexcept
{
    if (!ready_ || spec_.wire_rate == 0 || spec_.source_rate == 0) {
        return static_cast<double>(spec_.origin_source_frame);
    }

    // Where the device is now: what it said, plus what has happened since.
    const double elapsed = seconds_between(reading_.ticks, ticks, spec_.tick_rate);
    const double played = static_cast<double>(reading_.device_frames) +
                          elapsed * static_cast<double>(spec_.wire_rate);

    // Back to the anchor, and back again by what the chain is holding. The
    // frames leaving the endpoint went through the chain, so the source
    // material they came from is `latency_frames` earlier than the count.
    double since_origin = played - static_cast<double>(spec_.origin_device_frame) -
                          static_cast<double>(spec_.latency_frames);
    if (since_origin < 0.0) {
        since_origin = 0.0;
    }

    const double ratio =
        static_cast<double>(spec_.source_rate) / static_cast<double>(spec_.wire_rate);
    return static_cast<double>(spec_.origin_source_frame) + since_origin * ratio;
}

double AvClock::audible_seconds(std::uint64_t ticks) const noexcept
{
    if (spec_.source_rate == 0) {
        return 0.0;
    }
    return audible_frames(ticks) / static_cast<double>(spec_.source_rate);
}

double stream_seconds(std::uint64_t ticks, std::uint32_t timescale) noexcept
{
    if (timescale == 0) {
        return 0.0;
    }
    return static_cast<double>(ticks) / static_cast<double>(timescale);
}

void VideoPacer::configure(std::uint32_t timescale, std::uint32_t fps_num,
                           std::uint32_t fps_den) noexcept
{
    timescale_ = timescale;
    fps_num_ = fps_num;
    fps_den_ = fps_den;
    reset();
    stats_ = Stats{};
}

void VideoPacer::reset() noexcept
{
    measured_ = 0.0;
    last_pts_ = 0;
    have_last_ = false;
}

double VideoPacer::interval_seconds() const noexcept
{
    if (fps_num_ != 0 && fps_den_ != 0) {
        return static_cast<double>(fps_den_) / static_cast<double>(fps_num_);
    }
    return measured_;
}

VideoPacer::Decision VideoPacer::decide(std::uint64_t pts_ticks,
                                        double audible_seconds) noexcept
{
    Decision out;

    if (timescale_ == 0) {
        // A stream that did not say what its timestamps are counted in cannot
        // be placed against a clock, and §9.9 records that zero means "not
        // stated" rather than "zero". Both demuxers here always state it, so
        // this is a guard and not a path -- and showing every frame as it
        // decodes is the only thing left that is not a guess.
        ++stats_.shown;
        return out;
    }

    const double due = stream_seconds(pts_ticks, timescale_) + skew_;
    out.error_seconds = due - audible_seconds;

    if (out.error_seconds > 0.0) {
        // Its turn has not come. Nothing is recorded: the caller will ask
        // again about this same frame, and the picture on screen stays up.
        out.fate = FrameFate::repeat;
        return out;
    }

    const double interval = interval_seconds();
    if (interval > 0.0 && -out.error_seconds > interval) {
        out.fate = FrameFate::drop;
        ++stats_.dropped;
    } else {
        out.fate = FrameFate::show;
        ++stats_.shown;
    }

    // Learned from the frames that were consumed, in the order they were
    // consumed, which is presentation order -- the only order in which a
    // difference between two timestamps is an interval at all.
    if (have_last_ && pts_ticks > last_pts_) {
        measured_ = stream_seconds(pts_ticks - last_pts_, timescale_);
    }
    last_pts_ = pts_ticks;
    have_last_ = true;
    return out;
}

} // namespace mp

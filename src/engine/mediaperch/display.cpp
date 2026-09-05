// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/display.hpp"

namespace mp {

namespace {

/// Whether two specs describe the same run from the same place.
///
/// Only the anchor moves while a graph is playing; the rates and the latency
/// are settled when the graph is built. Comparing all of it anyway costs
/// nothing and means a rebuild that changed one of them is not missed.
bool same(const ClockSpec& a, const ClockSpec& b) noexcept
{
    return a.wire_rate == b.wire_rate && a.source_rate == b.source_rate &&
           a.tick_rate == b.tick_rate && a.latency_frames == b.latency_frames &&
           a.origin_device_frame == b.origin_device_frame &&
           a.origin_source_frame == b.origin_source_frame;
}

} // namespace

DisplayLoop::DisplayLoop(VideoGraph& graph, IAudioClockSource& audio,
                         IFrameClock& frames) noexcept
    : graph_(&graph), audio_(&audio), frames_(&frames)
{
}

void DisplayLoop::refresh_spec()
{
    ClockSpec spec = audio_->spec();
    // The graphs leave this zero because the counter is the caller's; the frame
    // clock is the caller, and its ticks are what the readings are stamped
    // with. Filled here so the two cannot come from different counters.
    spec.tick_rate = frames_->rate();
    if (configured_ && same(spec, spec_)) {
        return;
    }
    // **A reading taken before the anchor moved describes the old run.**
    // `configure` forgets it, and the next `observe` in this same turn puts a
    // fresh one in -- so a seek costs one turn of no clock rather than one turn
    // of the wrong one.
    spec_ = spec;
    clock_.configure(spec_);
    if (configured_) {
        ++stats_.reanchored;
    }
    configured_ = true;
}

bool DisplayLoop::once(DisplayStep& out)
{
    out = DisplayStep{};
    if (!frames_->wait()) {
        return false;
    }
    ++stats_.turns;

    refresh_spec();
    ClockReading reading{};
    if (audio_->read(reading)) {
        clock_.observe(reading);
    }

    if (!clock_.ready()) {
        // Nothing is playing, or the sink has no clock. Either way there is no
        // master to decide against, and §8 says a picture is not drawn against
        // a guess. The one already up stays up.
        ++stats_.without_clock;
        out.step = VideoGraph::Step::repeated;
        return !graph_->finished();
    }

    out.had_clock = true;
    out.step = graph_->pump(clock_.audible_seconds(frames_->now()));
    return out.step != VideoGraph::Step::finished &&
           out.step != VideoGraph::Step::failed;
}

std::uint64_t DisplayLoop::run()
{
    DisplayStep step;
    while (once(step)) {
    }
    return stats_.turns;
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/display.hpp"

#include <cmath>

namespace mp {

namespace {

/// The window a gap has to fall in to be a refresh at all. Half a millisecond
/// is 2000 Hz and a fifth of a second is 5 Hz; outside those a turn is not
/// describing a display, and no display this wide is worth admitting for the
/// sake of one that does not exist.
constexpr double k_gap_shortest = 0.0005;
constexpr double k_gap_longest = 0.2;

/// A gap below this fraction of the shortest seen means the shortest was not
/// one refresh. Three quarters, because the smallest way to be wrong is by a
/// factor of two: a true refresh against a two-refresh scale reads as a half.
constexpr double k_scale_wrong = 0.75;

/// How many refreshes a gap may span and still be counted. Eight is a turn that
/// missed seven vertical blanks, which is a stall rather than a measurement --
/// and the further out the multiple, the more the scale's own error moves the
/// rounding.
constexpr long long k_span_most = 8;

/// How near a whole number of refreshes a gap has to land. A quarter is far
/// looser than jitter needs and far tighter than picking the wrong integer
/// requires, which is the point: it rejects gaps that are not multiples at all
/// without rejecting noisy ones.
constexpr double k_span_tolerance = 0.25;

/// Refreshes of span before the average is trusted over the shortest gap. Two,
/// because the average is unbiased from the first one and only gets better,
/// while the shortest gap's error is there from the start and stays.
constexpr std::uint64_t k_span_enough = 2;

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

double DisplayLoop::interval_now() const noexcept
{
    if (span_refreshes_ >= k_span_enough && span_seconds_ > 0.0) {
        return span_seconds_ / static_cast<double>(span_refreshes_);
    }
    return shortest_gap_;
}

void DisplayLoop::learn_refresh(std::uint64_t ticks)
{
    const std::uint64_t rate = frames_->rate();
    if (have_last_tick_ && ticks > last_tick_ && rate != 0) {
        const double gap =
            static_cast<double>(ticks - last_tick_) / static_cast<double>(rate);
        // A gap shorter than half a millisecond is not a display and a gap
        // longer than a fifth of a second is a turn that was starved; neither
        // says anything about the refresh.
        if (gap > k_gap_shortest && gap < k_gap_longest) {
            if (shortest_gap_ == 0.0 || gap < shortest_gap_ * k_scale_wrong) {
                // **A gap this much shorter than anything before it means the
                // scale was wrong**, and everything counted against it counted
                // the wrong number of refreshes. It happens when the first gap
                // of a run is a starved one: nothing yet says a refresh is
                // shorter than that, so it is taken for one refresh and the
                // count is out by a factor until a real one arrives. Start the
                // span again from here rather than averaging over a lie.
                shortest_gap_ = gap;
                span_seconds_ = 0.0;
                span_refreshes_ = 0;
            } else if (gap < shortest_gap_) {
                shortest_gap_ = gap;
            }

            // How many refreshes this gap is, decided against what is known so
            // far. The rounding tolerates a lot because it only has to pick an
            // integer: the scale would have to be a quarter out to choose the
            // wrong one, and it is a percent out at worst.
            const double scale = interval_now();
            if (scale > 0.0) {
                // `spans` is a measurement and stays a double; **the count is a
                // count** and becomes an integer here and nowhere later. The
                // input is bounded -- the gap window above is 0.5 ms to 200 ms
                // and the scale cannot be under the same floor -- so `llround`
                // has at most 400 to represent and no way to overflow.
                const double spans = gap / scale;
                const long long whole = std::llround(spans);
                if (whole >= 1 && whole <= k_span_most &&
                    std::abs(spans - static_cast<double>(whole)) < k_span_tolerance) {
                    span_seconds_ += gap;
                    span_refreshes_ += static_cast<std::uint64_t>(whole);
                }
            }
        }
    }
    last_tick_ = ticks;
    have_last_tick_ = true;

    stats_.refresh_seconds = interval_now();
    stats_.refresh_span = span_refreshes_;

    // Until two turns have happened, whatever the display said about itself.
    const double interval = stats_.refresh_seconds != 0.0 ? stats_.refresh_seconds
                                                          : frames_->nominal_interval();
    graph_->set_lead_seconds(interval);
}

bool DisplayLoop::once(DisplayStep& out)
{
    out = DisplayStep{};
    if (!frames_->wait()) {
        return false;
    }
    ++stats_.turns;
    // Once, and used for both: the tick a frame is drawn at is the tick the
    // audio position is extrapolated to, and reading the counter twice would
    // put the difference between two reads in between them.
    const std::uint64_t tick = frames_->now();
    learn_refresh(tick);

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
    out.step = graph_->pump(clock_.audible_seconds(tick));
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

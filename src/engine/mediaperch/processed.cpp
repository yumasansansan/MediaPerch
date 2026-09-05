// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/processed.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mp {
namespace {

std::size_t ring_bytes(std::uint32_t period_frames, std::uint32_t frame_bytes,
                       std::uint32_t periods, std::size_t pump_bytes)
{
    const std::size_t bytes = static_cast<std::size_t>(period_frames) * frame_bytes * periods;
    // Room for two full pumps whatever else happens: one being written while
    // the other is being read is the least a ring can be and still be a ring.
    return std::max({bytes, 2 * pump_bytes, std::size_t{4096}});
}

} // namespace

ProcessedGraph::ProcessedGraph(ISource& source, Sink& sink, const Format& wire,
                               std::uint32_t period_frames, ConvertConfig convert,
                               IRenderThreadHooks* hooks, PassthroughConfig config,
                               DspChain* chain)
    : source_(&source), sink_(&sink), wire_(wire), source_format_(source.format()),
      proc_(source.format(), wire, period_frames, convert, chain), hooks_(hooks),
      config_(config), period_frames_(period_frames), wire_frame_bytes_(frame_bytes(wire)),
      source_frame_bytes_(frame_bytes(source.format())),
      pump_bytes_(proc_.output_bytes()),
      ring_(ring_bytes(period_frames, frame_bytes(wire), config.ring_periods, pump_bytes_))
{
    source_chunk_.resize(proc_.input_bytes());
    converted_chunk_.resize(pump_bytes_);
}

ProcessedGraph::~ProcessedGraph()
{
    stop();
}

void ProcessedGraph::fail(MpResult r) noexcept
{
    MpResult expected = MP_OK;
    error_.compare_exchange_strong(expected, r, std::memory_order_relaxed);
    running_.store(false, std::memory_order_release);
}

bool ProcessedGraph::pump_once()
{
    const std::size_t got = source_->read(source_chunk_.data(), source_chunk_.size());
    const std::size_t frames = got / source_frame_bytes_;
    std::uint32_t produced = 0;

    if (frames == 0) {
        // The source is finished. What the chain still holds is not: a
        // resampler keeps a filter's worth of the last audio and it is as much
        // the file as the rest.
        const bool again = proc_.flush(converted_chunk_.data(), produced);
        if (produced != 0) {
            ring_.write(converted_chunk_.data(),
                        static_cast<std::size_t>(produced) * wire_frame_bytes_);
        }
        return again;
    }

    frames_decoded_.fetch_add(frames, std::memory_order_relaxed);
    if (!proc_.run(source_chunk_.data(), static_cast<std::uint32_t>(frames),
                   converted_chunk_.data(), produced)) {
        fail(MP_ERR_INTERNAL);
        return false;
    }
    // 0 is normal: a stage may still be filling its history.
    if (produced != 0) {
        ring_.write(converted_chunk_.data(),
                    static_cast<std::size_t>(produced) * wire_frame_bytes_);
    }
    return true;
}

MpResult ProcessedGraph::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return MP_ERR_BUSY;
    }
    // A chain configured for something other than what it is about to be
    // handed is a bug in the caller, and one that would otherwise show up as
    // noise rather than as an error. `Processor::possible` is where that and
    // every other shape check now lives.
    if (!proc_.possible() || period_frames_ == 0) {
        return MP_ERR_INVALID;
    }
    if (!*sink_) {
        return MP_ERR_INVALID;
    }

    ring_.reset();
    // Put the drain back, so a graph started a second time finds the end of the
    // stream again rather than believing it has already been there.
    (void)proc_.reset();
    drained_.store(false, std::memory_order_release);
    error_.store(MP_OK, std::memory_order_relaxed);

    while (ring_.writable() >= pump_bytes_) {
        if (!pump_once()) {
            drained_.store(true, std::memory_order_release);
            break;
        }
    }

    void* buffer = nullptr;
    std::uint32_t frames = 0;
    MpResult r = sink_->acquire(buffer, frames);
    if (r != MP_OK) {
        return r;
    }
    const std::size_t want = static_cast<std::size_t>(frames) * wire_frame_bytes_;
    const std::size_t filled = ring_.read(buffer, want);
    if (filled < want) {
        std::memset(static_cast<std::uint8_t*>(buffer) + filled, 0, want - filled);
        // A file shorter than one device period, which is 3 ms. Rare, real, and
        // silence at the start rather than at the end.
        silent_frames_.fetch_add((want - filled) / wire_frame_bytes_,
                                 std::memory_order_relaxed);
    }
    r = sink_->commit(frames, 0);
    if (r != MP_OK) {
        return r;
    }
    // **Counted here, not only in the render loop.** This period is played:
    // `Initialize` allocates two buffers and this fills the first one, which is
    // what the device begins with. Leaving it out made every report in this
    // program one period short of what the device was actually handed -- 3 ms,
    // consistently, which is exactly the size of thing that gets explained away
    // as a rounding error rather than counted.
    frames_rendered_.fetch_add(frames, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);
    device_running_ = true;

    r = sink_->start();
    if (r != MP_OK) {
        running_.store(false, std::memory_order_release);
        return r;
    }

    render_thread_ = std::thread([this] { render_loop(); });
    decode_thread_ = std::thread([this] { decode_loop(); });
    return MP_OK;
}

void ProcessedGraph::stop() noexcept
{
    running_.store(false, std::memory_order_release);
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    if (*sink_) {
        sink_->stop();
    }
}

void ProcessedGraph::decode_loop()
{
    const auto nap = std::chrono::microseconds{
        std::max<std::uint64_t>(250, (std::uint64_t{period_frames_} * 1'000'000ULL) /
                                             std::max(wire_.sample_rate, 1u) / 4)};

    while (running_.load(std::memory_order_acquire)) {
        const std::uint64_t target = seek_request_.load(std::memory_order_acquire);
        if (target != k_no_seek) {
            perform_seek(target);
            continue;
        }
        if (drained_.load(std::memory_order_acquire) || ring_.writable() < pump_bytes_) {
            std::this_thread::sleep_for(nap);
            continue;
        }
        if (!pump_once()) {
            drained_.store(true, std::memory_order_release);
        }
    }
}

/// Byte for byte the same loop as Path A's, and that is the point: whatever the
/// decode thread did to the samples, the thread with the 3 ms deadline copies
/// bytes out of a ring and knows nothing else.
void ProcessedGraph::render_loop() noexcept
{
    if (hooks_ != nullptr) {
        hooks_->enter();
    }

    while (running_.load(std::memory_order_acquire)) {
        render_tick_.fetch_add(1, std::memory_order_release);

        // Paused: the device is stopped rather than fed silence. It is ours
        // either way in exclusive mode, and a clock that is not running is what
        // makes resuming land on the sample it left.
        if (paused_.load(std::memory_order_acquire)) {
            if (device_running_) {
                sink_->stop();
                device_running_ = false;
            }
            parked_.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            continue;
        }
        if (!device_running_) {
            const MpResult restarted = sink_->start();
            if (restarted != MP_OK) {
                fail(restarted);
                break;
            }
            device_running_ = true;
        }

        const MpResult waited = sink_->wait(config_.wait_timeout_ms);
        if (waited == MP_TIMEOUT) {
            wait_timeouts_.fetch_add(1, std::memory_order_relaxed);
            fail(MP_TIMEOUT);
            break;
        }
        if (waited != MP_OK) {
            fail(waited);
            break;
        }

        void* buffer = nullptr;
        std::uint32_t frames = 0;
        const MpResult acquired = sink_->acquire(buffer, frames);
        if (acquired != MP_OK) {
            fail(acquired);
            break;
        }

        const std::size_t want = static_cast<std::size_t>(frames) * wire_frame_bytes_;
        // A seek is in progress: the ring belongs to the decode thread for the
        // moment, so this writes silence and keeps the clock running. Ten
        // milliseconds of it, and the device never learns anything happened.
        const bool seeking = seeking_.load(std::memory_order_acquire);
        parked_.store(seeking, std::memory_order_release);
        const std::size_t got = seeking ? 0 : ring_.read(buffer, want);
        if (got < want) {
            std::memset(static_cast<std::uint8_t*>(buffer) + got, 0, want - got);
        }

        const MpResult committed = sink_->commit(frames, 0);
        if (committed != MP_OK) {
            fail(committed);
            break;
        }
        frames_rendered_.fetch_add(frames, std::memory_order_relaxed);

        // Nothing left to play and nothing coming. Never while seeking: the
        // ring is empty because somebody emptied it, not because the file ended.
        const bool finishing = !seeking && drained_.load(std::memory_order_acquire) &&
                               ring_.readable() < wire_frame_bytes_;
        if (got < want && !seeking) {
            // **A file's last period is not an underrun.** The device is owed a
            // whole period and the file ended in the middle of one, so the rest
            // is padded -- which is the end of the stream, not starvation, and
            // counting it as starvation would make the underrun count useless
            // for the only thing anybody plays. Asked after the commit rather
            // than before it, which is as much time as can be given to the
            // decode thread to have published that it finished.
            auto& counter = finishing ? tail_frames_ : silent_frames_;
            counter.fetch_add((want - got) / wire_frame_bytes_, std::memory_order_relaxed);
            if (!finishing) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (finishing) {
            running_.store(false, std::memory_order_release); // rather than silence for ever
        }
    }

    if (hooks_ != nullptr) {
        hooks_->leave();
    }
}

ProcessedGraph::Stats ProcessedGraph::stats() const noexcept
{
    Stats out;
    out.frames_rendered = frames_rendered_.load(std::memory_order_relaxed);
    out.underruns = underruns_.load(std::memory_order_relaxed);
    out.silent_frames = silent_frames_.load(std::memory_order_relaxed);
    out.tail_frames = tail_frames_.load(std::memory_order_relaxed);
    out.wait_timeouts = wait_timeouts_.load(std::memory_order_relaxed);
    out.frames_decoded = frames_decoded_.load(std::memory_order_relaxed);
    return out;
}


void ProcessedGraph::pause() noexcept
{
    paused_.store(true, std::memory_order_release);
}

void ProcessedGraph::resume() noexcept
{
    paused_.store(false, std::memory_order_release);
}

std::uint64_t ProcessedGraph::position_frames() const noexcept
{
    const std::uint64_t rendered = frames_rendered_.load(std::memory_order_relaxed);
    const std::uint64_t base = rendered_base_.load(std::memory_order_relaxed);
    const std::uint64_t since = rendered > base ? rendered - base : 0;
    // The device counts in wire frames and the file counts in its own. They are
    // the same number unless something in the chain changed the rate, and then
    // they are not, so the ratio is applied rather than assumed away.
    const std::uint64_t source_rate = source_format_.sample_rate;
    const std::uint64_t wire_rate = wire_.sample_rate;
    const std::uint64_t scaled =
        wire_rate == 0 ? since : since * source_rate / wire_rate;
    return played_base_.load(std::memory_order_relaxed) + scaled;
}

ClockSpec ProcessedGraph::clock_spec() const noexcept
{
    ClockSpec out;
    out.wire_rate = wire_.sample_rate;
    out.source_rate = source_format_.sample_rate;
    out.tick_rate = 0; // the caller's counter, not this graph's
    out.latency_frames = proc_.latency_frames();
    // The pair the position accounting has always kept: at the device frame
    // that had been written when the run began or the last seek landed, the
    // source was at `played_base_`.
    out.origin_device_frame = rendered_base_.load(std::memory_order_relaxed);
    out.origin_source_frame = played_base_.load(std::memory_order_relaxed);
    return out;
}

bool ProcessedGraph::read_clock(ClockReading& out) noexcept
{
    std::uint64_t frames = 0;
    std::uint64_t ticks = 0;
    if (sink_ == nullptr || sink_->position(frames, ticks) != MP_OK) {
        return false;
    }
    out.device_frames = frames;
    out.ticks = ticks;
    return true;
}

bool ProcessedGraph::seek(std::uint64_t frame)
{
    if (!source_->seekable()) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        // Nothing is playing, so there is nobody to hand the ring to.
        perform_seek(frame);
        return true;
    }
    seek_request_.store(frame, std::memory_order_release);
    // Wait for the decode thread to have done it. A seek whose end a caller
    // cannot observe is one a caller has to guess about.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (seek_request_.load(std::memory_order_acquire) != k_no_seek &&
           running_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

void ProcessedGraph::perform_seek(std::uint64_t frame)
{
    // Take the ring. The render thread answers by parking; a whole iteration
    // under the flag is what says it is no longer inside `ring_.read`, and
    // resetting a ring underneath a reader is how a seek becomes a burst of
    // whatever was in memory.
    seeking_.store(true, std::memory_order_release);
    const std::uint64_t from = render_tick_.load(std::memory_order_acquire);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (running_.load(std::memory_order_acquire)) {
        if (parked_.load(std::memory_order_acquire) &&
            render_tick_.load(std::memory_order_acquire) >= from + 2) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            break; // the render thread is gone; the seek is still the right thing
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    ring_.reset();
    const bool moved = source_->seek(frame);
    // Every filter in the chain is holding samples from where the stream used
    // to be, and those are no longer adjacent to what comes next. This also
    // puts the drain back, so the end of the stream is found again.
    (void)proc_.reset();
    drained_.store(false, std::memory_order_release);
    if (moved) {
        played_base_.store(frame, std::memory_order_relaxed);
        rendered_base_.store(frames_rendered_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    }
    while (ring_.writable() >= pump_bytes_) {
        if (!pump_once()) {
            drained_.store(true, std::memory_order_release);
            break;
        }
    }
    seek_request_.store(k_no_seek, std::memory_order_release);
    seeking_.store(false, std::memory_order_release);
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/passthrough.hpp"

#include "mediaperch/repack.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mp {
namespace {

std::size_t round_up_ring(std::uint32_t period_frames, std::uint32_t frame_bytes,
                          std::uint32_t periods)
{
    const std::size_t bytes = static_cast<std::size_t>(period_frames) * frame_bytes * periods;
    return std::max<std::size_t>(bytes, 4096);
}

} // namespace

PassthroughGraph::PassthroughGraph(ISource& source, Sink& sink, const Format& wire,
                                   std::uint32_t period_frames, Fidelity fidelity,
                                   IRenderThreadHooks* hooks, PassthroughConfig config)
    : source_(&source), sink_(&sink), wire_(wire), source_format_(source.format()),
      fidelity_(fidelity), hooks_(hooks), config_(config), period_frames_(period_frames),
      wire_frame_bytes_(frame_bytes(wire)), source_frame_bytes_(frame_bytes(source.format())),
      chunk_frames_(period_frames),
      ring_(round_up_ring(period_frames, frame_bytes(wire), config.ring_periods))
{
    source_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * source_frame_bytes_);
    if (fidelity_ == Fidelity::repacked) {
        repacked_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_);
    }
}

PassthroughGraph::~PassthroughGraph()
{
    stop();
}

void PassthroughGraph::fail(MpResult r) noexcept
{
    MpResult expected = MP_OK;
    error_.compare_exchange_strong(expected, r, std::memory_order_relaxed);
    running_.store(false, std::memory_order_release);
}

bool PassthroughGraph::pump_once()
{
    const std::size_t got = source_->read(source_chunk_.data(), source_chunk_.size());
    if (got == 0) {
        return false;
    }

    const std::size_t frames = got / source_frame_bytes_;
    frames_decoded_.fetch_add(frames, std::memory_order_relaxed);

    if (fidelity_ == Fidelity::exact) {
        ring_.write(source_chunk_.data(), got);
        return true;
    }

    // Repacked: the same bits moved into a different container. Done here on the
    // decode thread, never on the render thread.
    const std::size_t samples = frames * wire_.channels;
    if (!repack(source_chunk_.data(), source_format_.sample_type, repacked_chunk_.data(),
                wire_.sample_type, effective_valid_bits(wire_), samples)) {
        fail(MP_ERR_FORMAT);
        return false;
    }
    ring_.write(repacked_chunk_.data(), repacked_bytes(wire_.sample_type, samples));
    return true;
}

MpResult PassthroughGraph::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return MP_ERR_BUSY;
    }
    if (fidelity_ == Fidelity::converted || wire_frame_bytes_ == 0 ||
        source_frame_bytes_ == 0 || period_frames_ == 0) {
        return MP_ERR_INVALID;
    }
    if (!*sink_) {
        return MP_ERR_INVALID;
    }

    ring_.reset();
    drained_.store(false, std::memory_order_release);
    error_.store(MP_OK, std::memory_order_relaxed);

    // Fill the ring before anything starts running, so the first device period
    // is never served from an empty ring.
    while (ring_.writable() >= static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_) {
        if (!pump_once()) {
            drained_.store(true, std::memory_order_release);
            break;
        }
    }

    // And prefill the device's own first buffer, which is what the Microsoft
    // sample does and what keeps the very first period from being silence.
    void* buffer = nullptr;
    std::uint32_t frames = 0;
    MpResult r = sink_->acquire(buffer, frames);
    if (r != MP_OK) {
        return r;
    }
    const std::size_t want = static_cast<std::size_t>(frames) * wire_frame_bytes_;
    const std::size_t got = ring_.read(buffer, want);
    if (got < want) {
        std::memset(static_cast<std::uint8_t*>(buffer) + got, 0, want - got);
        // A file shorter than one device period, which is 3 ms. Rare, real, and
        // silence at the start rather than at the end.
        silent_frames_.fetch_add((want - got) / wire_frame_bytes_,
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

    r = sink_->start();
    if (r != MP_OK) {
        running_.store(false, std::memory_order_release);
        return r;
    }

    render_thread_ = std::thread([this] { render_loop(); });
    decode_thread_ = std::thread([this] { decode_loop(); });
    return MP_OK;
}

void PassthroughGraph::stop() noexcept
{
    running_.store(false, std::memory_order_release);

    // The render thread first: nothing else may touch the device buffer while it
    // might still be inside acquire/commit.
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

void PassthroughGraph::decode_loop()
{
    const std::size_t chunk_bytes = static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_;

    // A quarter of a period is short enough that the ring never runs dry waiting
    // for this thread, and long enough that it is not a spin.
    const auto nap = std::chrono::microseconds{
        std::max<std::uint64_t>(250, (std::uint64_t{period_frames_} * 1'000'000ULL) /
                                             std::max(wire_.sample_rate, 1u) / 4)};

    while (running_.load(std::memory_order_acquire)) {
        if (drained_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(nap);
            continue;
        }
        if (ring_.writable() < chunk_bytes) {
            std::this_thread::sleep_for(nap);
            continue;
        }
        if (!pump_once()) {
            drained_.store(true, std::memory_order_release);
        }
    }
}

void PassthroughGraph::render_loop() noexcept
{
    if (hooks_ != nullptr) {
        hooks_->enter();
    }

    while (running_.load(std::memory_order_acquire)) {
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
        const std::size_t got = ring_.read(buffer, want);
        if (got < want) {
            std::memset(static_cast<std::uint8_t*>(buffer) + got, 0, want - got);
        }

        const MpResult committed = sink_->commit(frames, 0);
        if (committed != MP_OK) {
            fail(committed);
            break;
        }
        frames_rendered_.fetch_add(frames, std::memory_order_relaxed);

        // Nothing left to play and nothing coming.
        const bool finishing =
            drained_.load(std::memory_order_acquire) && ring_.readable() < wire_frame_bytes_;
        if (got < want) {
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

PassthroughGraph::Stats PassthroughGraph::stats() const noexcept
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

} // namespace mp

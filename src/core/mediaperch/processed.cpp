// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/processed.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mp {
namespace {

std::size_t ring_bytes(std::uint32_t period_frames, std::uint32_t frame_bytes,
                       std::uint32_t periods)
{
    const std::size_t bytes = static_cast<std::size_t>(period_frames) * frame_bytes * periods;
    return std::max<std::size_t>(bytes, 4096);
}

} // namespace

ProcessedGraph::ProcessedGraph(ISource& source, Sink& sink, const Format& wire,
                               std::uint32_t period_frames, ConvertConfig convert,
                               IRenderThreadHooks* hooks, PassthroughConfig config)
    : source_(&source), sink_(&sink), wire_(wire), source_format_(source.format()),
      converter_(source.format(), wire, convert), hooks_(hooks), config_(config),
      period_frames_(period_frames), wire_frame_bytes_(frame_bytes(wire)),
      source_frame_bytes_(frame_bytes(source.format())), chunk_frames_(period_frames),
      ring_(ring_bytes(period_frames, frame_bytes(wire), config.ring_periods))
{
    source_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * source_frame_bytes_);
    converted_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_);
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
    if (got == 0) {
        return false;
    }
    const std::size_t frames = got / source_frame_bytes_;
    if (frames == 0) {
        return false;
    }
    frames_decoded_.fetch_add(frames, std::memory_order_relaxed);

    // The whole of Path B, in one call, on the decode thread.
    converter_.run(source_chunk_.data(), converted_chunk_.data(), frames);
    ring_.write(converted_chunk_.data(), frames * wire_frame_bytes_);
    return true;
}

MpResult ProcessedGraph::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return MP_ERR_BUSY;
    }
    if (!converter_.possible() || wire_frame_bytes_ == 0 || source_frame_bytes_ == 0 ||
        period_frames_ == 0) {
        return MP_ERR_INVALID;
    }
    if (!*sink_) {
        return MP_ERR_INVALID;
    }

    ring_.reset();
    drained_.store(false, std::memory_order_release);
    error_.store(MP_OK, std::memory_order_relaxed);

    while (ring_.writable() >= static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_) {
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
    }
    r = sink_->commit(frames, 0);
    if (r != MP_OK) {
        return r;
    }

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
    const std::size_t chunk_bytes = static_cast<std::size_t>(chunk_frames_) * wire_frame_bytes_;
    const auto nap = std::chrono::microseconds{
        std::max<std::uint64_t>(250, (std::uint64_t{period_frames_} * 1'000'000ULL) /
                                             std::max(wire_.sample_rate, 1u) / 4)};

    while (running_.load(std::memory_order_acquire)) {
        if (drained_.load(std::memory_order_acquire) || ring_.writable() < chunk_bytes) {
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
            underruns_.fetch_add(1, std::memory_order_relaxed);
            silent_frames_.fetch_add((want - got) / wire_frame_bytes_,
                                     std::memory_order_relaxed);
        }

        const MpResult committed = sink_->commit(frames, 0);
        if (committed != MP_OK) {
            fail(committed);
            break;
        }
        frames_rendered_.fetch_add(frames, std::memory_order_relaxed);

        if (drained_.load(std::memory_order_acquire) && ring_.readable() < wire_frame_bytes_) {
            running_.store(false, std::memory_order_release);
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
    out.wait_timeouts = wait_timeouts_.load(std::memory_order_relaxed);
    out.frames_decoded = frames_decoded_.load(std::memory_order_relaxed);
    return out;
}

} // namespace mp

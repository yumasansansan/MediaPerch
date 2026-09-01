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

/// What one `pump_once` can produce, in wire bytes.
std::uint32_t pump_bytes_for(const DspChain* chain, std::uint32_t chunk_frames,
                             std::uint32_t wire_frame_bytes)
{
    const std::uint32_t frames =
        chain != nullptr ? std::max(chain->output_capacity(), chunk_frames) : chunk_frames;
    return frames * wire_frame_bytes;
}

} // namespace

ProcessedGraph::ProcessedGraph(ISource& source, Sink& sink, const Format& wire,
                               std::uint32_t period_frames, ConvertConfig convert,
                               IRenderThreadHooks* hooks, PassthroughConfig config,
                               DspChain* chain)
    : source_(&source), sink_(&sink), wire_(wire), source_format_(source.format()),
      // The quantiser reads whatever reaches it: the source itself, or the
      // chain's output, which may be at a rate the source never had.
      converter_(chain != nullptr ? chain->output_format() : source.format(), wire, convert),
      chain_(chain), bus_(dsp_bus_format(source.format())),
      // Widening only: exact by construction, and nothing to dither because
      // nothing is being thrown away.
      to_bus_(source.format(), dsp_bus_format(source.format()), ConvertConfig{}),
      hooks_(hooks), config_(config),
      period_frames_(period_frames), wire_frame_bytes_(frame_bytes(wire)),
      source_frame_bytes_(frame_bytes(source.format())), chunk_frames_(period_frames),
      pump_bytes_(pump_bytes_for(chain, period_frames, frame_bytes(wire))),
      ring_(ring_bytes(period_frames, frame_bytes(wire), config.ring_periods, pump_bytes_))
{
    source_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * source_frame_bytes_);
    converted_chunk_.resize(pump_bytes_);
    if (chain_ != nullptr) {
        bus_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * frame_bytes(bus_));
    }
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
    if (frames == 0) {
        // The source is finished. What the chain still holds is not.
        return chain_ != nullptr && flush_chain();
    }
    frames_decoded_.fetch_add(frames, std::memory_order_relaxed);

    if (chain_ == nullptr) {
        // The common case: one conversion, on the decode thread.
        converter_.run(source_chunk_.data(), converted_chunk_.data(), frames);
        ring_.write(converted_chunk_.data(), frames * wire_frame_bytes_);
        return true;
    }

    // source -> the f64 bus -> the chain -> the wire format. The first step is
    // a widening and therefore exact; the last is the only quantiser in Path B,
    // which is what lets the dither and the shaping sit in one place.
    to_bus_.run(source_chunk_.data(), bus_chunk_.data(), frames);
    std::uint32_t produced = 0;
    if (!chain_->run(reinterpret_cast<const double*>(bus_chunk_.data()),
                     static_cast<std::uint32_t>(frames), chain_out_, produced)) {
        fail(MP_ERR_INTERNAL);
        return false;
    }
    emit(produced); // 0 is normal: a stage may still be filling its history
    return true;
}

void ProcessedGraph::emit(std::uint32_t frames)
{
    if (frames == 0) {
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(frames) * wire_frame_bytes_;
    if (converted_chunk_.size() < bytes) {
        converted_chunk_.resize(bytes);
    }
    converter_.run(chain_out_.data(), converted_chunk_.data(), frames);
    ring_.write(converted_chunk_.data(), bytes);
}

/// One round of the drain, because the ABI says a stage is flushed until it
/// says it is empty and because each round has to fit in the room the ring has.
bool ProcessedGraph::flush_chain()
{
    if (flushed_) {
        return false;
    }
    std::uint32_t produced = 0;
    if (!chain_->flush(chain_out_, produced)) {
        flushed_ = true;
        fail(MP_ERR_INTERNAL);
        return false;
    }
    emit(produced);
    flushed_ = chain_->flush_done();
    // Coming back for another round counts as having done something, even when
    // this round produced nothing: the stage behind this one may still be full.
    return !flushed_ || produced != 0;
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
    if (chain_ != nullptr &&
        (!to_bus_.possible() || chain_->input_format() != bus_ ||
         chain_->output_capacity() == 0)) {
        // A chain configured for something other than what it is about to be
        // handed is a bug in the caller, and one that would otherwise show up
        // as noise rather than as an error.
        return MP_ERR_INVALID;
    }
    if (!*sink_) {
        return MP_ERR_INVALID;
    }

    ring_.reset();
    flushed_ = false;
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
    const auto nap = std::chrono::microseconds{
        std::max<std::uint64_t>(250, (std::uint64_t{period_frames_} * 1'000'000ULL) /
                                             std::max(wire_.sample_rate, 1u) / 4)};

    while (running_.load(std::memory_order_acquire)) {
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

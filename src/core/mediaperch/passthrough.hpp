// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/negotiation.hpp"
#include "mediaperch/ring.hpp"
#include "mediaperch/sink.hpp"
#include "mediaperch/source.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace mp {

/// Tuning for the passthrough graph.
///
/// At namespace scope rather than nested inside PassthroughGraph, because a
/// default argument of a nested type needs that type's default member
/// initializers while the enclosing class is still incomplete. MSVC accepts it;
/// clang does not, and clang is right.
struct PassthroughConfig {
    /// Ring capacity, in device periods. Eight periods at a 3 ms period is about
    /// 24 ms of slack for a decode thread that is not real-time.
    std::uint32_t ring_periods = 8;
    std::uint32_t wait_timeout_ms = 2000;
};

/// Path A: the source, a ring, and the device. No float, no gain, no resampling.
///
/// Two threads. The decode thread reads the source and fills the ring, and may
/// block and allocate. The render thread does nothing but wait on the device,
/// copy out of the ring, and hand the buffer back -- no allocation, no locking,
/// no logging, no branch on anything another thread writes except one atomic
/// stop flag.
///
/// A short read from the ring is an underrun: the rest of the device buffer is
/// filled with silence and counted. It is never an exception and never a retry,
/// because both cost time the render thread does not have.
class PassthroughGraph {
public:
    /// Everything the render thread has to say about how it went. Read from any
    /// thread; each field is a relaxed atomic, so the set is not a consistent
    /// snapshot and does not need to be.
    struct Stats {
        std::uint64_t frames_rendered = 0;
        /// Device buffers that the ring could not fill completely.
        std::uint64_t underruns = 0;
        std::uint64_t silent_frames = 0;
        /// The device did not signal within the timeout. Usually means it went away.
        std::uint64_t wait_timeouts = 0;
        std::uint64_t frames_decoded = 0;
    };

    /// `wire` is what the sink accepted, and `fidelity` is what `negotiate_best`
    /// said about it. `Fidelity::converted` is rejected: that is Path B.
    PassthroughGraph(ISource& source, Sink& sink, const Format& wire,
                     std::uint32_t period_frames, Fidelity fidelity,
                     IRenderThreadHooks* hooks = nullptr, PassthroughConfig config = {});

    PassthroughGraph(const PassthroughGraph&) = delete;
    PassthroughGraph& operator=(const PassthroughGraph&) = delete;
    PassthroughGraph(PassthroughGraph&&) = delete;
    PassthroughGraph& operator=(PassthroughGraph&&) = delete;
    ~PassthroughGraph();

    /// Fills the ring, prefills the device's first buffer, starts the device and
    /// launches both threads. Returns what the sink said if any of that failed.
    MpResult start();

    /// Stops both threads and the device. Safe to call twice, and called by the
    /// destructor.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Set once by whichever thread hit it; MP_OK while nothing has gone wrong.
    [[nodiscard]] MpResult error() const noexcept
    {
        return error_.load(std::memory_order_relaxed);
    }

    /// True once the source returned end-of-stream and the ring has drained.
    [[nodiscard]] bool finished() const noexcept
    {
        return drained_.load(std::memory_order_acquire) &&
               ring_.readable() < wire_frame_bytes_;
    }

    [[nodiscard]] Stats stats() const noexcept;

private:
    void decode_loop();
    void render_loop() noexcept;
    void fail(MpResult r) noexcept;
    /// Reads one chunk from the source into the ring, repacking if the sink took
    /// a different container. Returns false at the end of the stream.
    bool pump_once();

    ISource* source_;
    Sink* sink_;
    Format wire_;
    Format source_format_;
    Fidelity fidelity_;
    IRenderThreadHooks* hooks_;
    PassthroughConfig config_;

    std::uint32_t period_frames_;
    std::uint32_t wire_frame_bytes_;
    std::uint32_t source_frame_bytes_;
    std::uint32_t chunk_frames_;

    ByteRing ring_;
    std::vector<std::uint8_t> source_chunk_;
    /// Only allocated when the sink took a different container.
    std::vector<std::uint8_t> repacked_chunk_;

    std::thread decode_thread_;
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> drained_{false};
    std::atomic<MpResult> error_{MP_OK};

    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> silent_frames_{0};
    std::atomic<std::uint64_t> wait_timeouts_{0};
    std::atomic<std::uint64_t> frames_decoded_{0};
};

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/avsync.hpp"
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
        /// Frames of silence written to fill the device's last period after the
        /// source ended. Expected, and not an underrun.
        std::uint64_t tail_frames = 0;
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

    // --- transport ---------------------------------------------------------
    //
    // Three operations, and each of them is about one thing: the device's clock
    // must never learn that anything happened. Pausing stops it deliberately;
    // seeking keeps it running on silence for the few milliseconds the decode
    // thread needs; and a track boundary is not here at all, because it belongs
    // to `Queue` and the graph never sees one.

    /// Stops the device rather than feeding it silence. In exclusive mode the
    /// device is ours either way, and a stopped clock is what makes resuming
    /// land on the sample it left.
    void pause() noexcept;
    void resume() noexcept;
    [[nodiscard]] bool paused() const noexcept
    {
        return paused_.load(std::memory_order_acquire);
    }

    /// Moves the source to `frame` and refills from there. Returns false when
    /// the source cannot seek at all; otherwise it returns once the audio has
    /// actually moved, because a seek a caller cannot observe the end of is a
    /// seek a caller has to guess about.
    [[nodiscard]] bool seek(std::uint64_t frame);

    /// Which source frame the device has been *given* -- not which one the
    /// decoder has reached, which is a ring's worth further ahead.
    ///
    /// **Given, not played**, and the difference is up to one device buffer.
    /// That is deliberate and design.md §"the resume point" is where it is
    /// argued: this number exists so a run interrupted by a device being pulled
    /// out can come back to where it was, and the device that could have said
    /// what it actually played is the one that just disappeared. For anything
    /// that needs what is *audible* -- §8's clock, and video against it -- use
    /// `clock_spec` and `read_clock`.
    [[nodiscard]] std::uint64_t position_frames() const noexcept;
    /// §8's master clock, as far as this graph can state it.
    ///
    /// A graph is the one thing that knows all of it: it owns the sink, it
    /// knows what the file counts in and what the device counts in, it built
    /// the chain, and it has kept the anchor since gapless was written.
    /// `AvClock` is what turns this plus a reading into a time.
    [[nodiscard]] ClockSpec clock_spec() const noexcept;

    /// One reading of the device's clock. False when the sink module has none
    /// -- and then there is no master clock, so nothing may be synchronised to
    /// it. `mp::Sink::position` says why that is a question rather than a zero.
    [[nodiscard]] bool read_clock(ClockReading& out) noexcept;


    /// Where the source already is, before anything plays.
    ///
    /// A run does not always begin at frame zero: `--seek` starts one part way
    /// in, and a run that resumes after the device was pulled out begins
    /// wherever the last one stopped. A position that pretended otherwise would
    /// be one nobody could seek back to. Call it before `start`.
    void set_position(std::uint64_t frame) noexcept
    {
        played_base_.store(frame, std::memory_order_relaxed);
    }


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

    /// Takes the ring from the render thread, moves the source, and refills.
    void perform_seek(std::uint64_t frame);

    /// No seek is pending. Not zero, because zero is the top of the file.
    static constexpr std::uint64_t k_no_seek = ~std::uint64_t{0};

    std::atomic<bool> paused_{false};
    /// Set by the decode thread while it owns the ring. The render thread
    /// answers by parking, and the tick is how the decode thread knows it did:
    /// a whole render iteration under the flag means nothing is inside
    /// `ring_.read` any more.
    std::atomic<bool> seeking_{false};
    std::atomic<bool> parked_{false};
    std::atomic<std::uint64_t> render_tick_{0};
    std::atomic<std::uint64_t> seek_request_{k_no_seek};
    /// Where the position is counted from, reset at every seek.
    std::atomic<std::uint64_t> played_base_{0};
    std::atomic<std::uint64_t> rendered_base_{0};
    /// Whether the device is started. Only the render thread touches it, and
    /// only while `stop()` is not running -- which joins first.
    bool device_running_ = false;


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
    std::atomic<std::uint64_t> tail_frames_{0};
    std::atomic<std::uint64_t> wait_timeouts_{0};
    std::atomic<std::uint64_t> frames_decoded_{0};
};

} // namespace mp

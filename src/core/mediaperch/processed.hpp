// SPDX-License-Identifier: GPL-3.0-or-later
//
// Path B: the graph that is allowed to change the samples.
//
// It exists because Path A can refuse. Every lossy decoder in this tree reports
// `F32`, because that is what a lossy codec's output is, and a great many
// endpoints will not take float in exclusive mode -- the one this was written on
// does not. Without this graph those files decode and hash and cannot be played.
//
// **The render thread is the same as Path A's, deliberately.** It waits on the
// device, copies out of the ring and commits, and that is all; it has no branch
// on anything, no float, and no idea a conversion happened. The conversion runs
// on the decode thread, in `Converter`, exactly where the repack runs in Path A.
// Two graphs rather than one with a branch is what §2 of the design asks for,
// and what that buys is precisely this: the 3 ms thread cannot take a wrong
// turn, because it has no turn to take.
//
// What it converts is narrow on purpose -- sample type, and a gain. It does not
// resample and does not change the channel count, so negotiation still refuses a
// device that wants a different rate. A resampler is a real component and a bad
// one would be worse than the refusal it replaced.

#ifndef MEDIAPERCH_PROCESSED_HPP
#define MEDIAPERCH_PROCESSED_HPP

#include "mediaperch/convert.hpp"
#include "mediaperch/dsp.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/ring.hpp"
#include "mediaperch/sink.hpp"
#include "mediaperch/source.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace mp {

/// Path B: the source, a conversion, a ring, and the device.
class ProcessedGraph {
public:
    using Stats = PassthroughGraph::Stats;

    /// `wire` is what the sink accepted. Unlike Path A it may be anything the
    /// converter can reach, which is any PCM sample type at the same rate and
    /// channel count.
    /// `chain` may be null, and usually is: a stream that only needs its
    /// sample type changed goes straight through one converter. When it is
    /// given, the shape becomes source -> f64 bus -> chain -> wire, with the
    /// widening exact and the one quantiser still at the end. It must already
    /// be configured, for `dsp_bus_format(source.format())` -- the graph reads
    /// its output format to build the converter behind it, and checks in
    /// `start` that it was configured for the stream it is about to be fed.
    ProcessedGraph(ISource& source, Sink& sink, const Format& wire,
                   std::uint32_t period_frames, ConvertConfig convert = {},
                   IRenderThreadHooks* hooks = nullptr, PassthroughConfig config = {},
                   DspChain* chain = nullptr);

    ProcessedGraph(const ProcessedGraph&) = delete;
    ProcessedGraph& operator=(const ProcessedGraph&) = delete;
    ProcessedGraph(ProcessedGraph&&) = delete;
    ProcessedGraph& operator=(ProcessedGraph&&) = delete;
    ~ProcessedGraph();

    MpResult start();
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

    /// Which source frame the device is playing now -- not which one the
    /// decoder has reached, which is a ring's worth ahead of what anybody can
    /// hear.
    [[nodiscard]] std::uint64_t position_frames() const noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] MpResult error() const noexcept
    {
        return error_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool finished() const noexcept
    {
        return drained_.load(std::memory_order_acquire) &&
               ring_.readable() < wire_frame_bytes_;
    }
    [[nodiscard]] Stats stats() const noexcept;

    /// What the conversion is doing, for whoever has to tell the user. A graph
    /// that is not lossy is one where the only change was a gain of exactly one
    /// -- worth saying, because "processed" and "altered" are not the same word.
    [[nodiscard]] bool lossy() const noexcept { return converter_.lossy(); }

private:
    void decode_loop();
    void render_loop() noexcept;
    void fail(MpResult r) noexcept;
    bool pump_once();
    /// Converts `frames` from `chain_out_` and puts them in the ring.
    void emit(std::uint32_t frames);
    /// Drains the chain once, at the end of the stream. A resampler holds a
    /// filter's worth of the last audio and it is as much the file as the rest.
    bool flush_chain();

    ISource* source_;
    Sink* sink_;
    Format wire_;
    Format source_format_;
    /// Without a chain: source straight to the wire format.
    /// With one: source to the bus, and the bus to the wire format.
    Converter converter_;
    DspChain* chain_;
    Format bus_{};
    Converter to_bus_;
    std::vector<std::uint8_t> bus_chunk_;
    std::vector<double> chain_out_;
    IRenderThreadHooks* hooks_;
    PassthroughConfig config_;

    std::uint32_t period_frames_;
    std::uint32_t wire_frame_bytes_;
    std::uint32_t source_frame_bytes_;
    std::uint32_t chunk_frames_;
    /// The most one `pump_once` can put in the ring. Without a chain that is
    /// one chunk; with one it is whatever the chain said it could produce,
    /// which for anything that upsamples is more. Both loops keep this much
    /// room free, so a write never has to be split or dropped.
    std::uint32_t pump_bytes_;

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
    std::vector<std::uint8_t> converted_chunk_;

    std::thread decode_thread_;
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> drained_{false};
    bool flushed_ = false;
    std::atomic<MpResult> error_{MP_OK};

    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> silent_frames_{0};
    std::atomic<std::uint64_t> tail_frames_{0};
    std::atomic<std::uint64_t> wait_timeouts_{0};
    std::atomic<std::uint64_t> frames_decoded_{0};
};

} // namespace mp

#endif // MEDIAPERCH_PROCESSED_HPP

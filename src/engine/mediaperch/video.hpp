// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The video half of a player: a decoder, a presenter, and §8 between them.
//
// **One frame in hand, and no queue.** A decoded frame is valid until the next
// call on the codec that produced it -- the ABI says so, and it says so because
// a hardware decoder hands out a slice of a pool it owns. A graph that queued
// frames ahead would have to copy them, which is exactly what §9.8.1's
// texture-adoption argument exists to avoid. So this holds one frame, decides
// about it, and asks for the next only once it is done with that one. The
// lookahead that a decode-ahead queue would have bought is already inside the
// decoder, which reorders B-frames and runs on every core the machine has.
//
// **It owns no thread**, and that is the difference from the audio graphs. They
// own one because the device's own event is what paces them: a render thread
// with a 3 ms deadline waits on the sink and nothing else. Video's pace is the
// display's, and a display belongs to the head -- DirectComposition, a swap
// chain's waitable object, a vblank. So `pump` is a call, and whoever owns the
// display loop makes it.

#include "mediaperch/avsync.hpp"
#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <cstdint>
#include <vector>

namespace mp {

/// A video codec module, behind the C vtable.
///
/// Named for what it is rather than `MpVideoCodec`, which is the ABI's opaque
/// handle and is a type each module defines for itself.
class VideoDecoder final {
public:
    VideoDecoder() noexcept = default;
    ~VideoDecoder() { close(); }

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    /// `device` is the presenter's, or null for a decoder that works in system
    /// memory. §9.8.1 is why it is passed rather than made: a decoder that
    /// creates its own D3D11 device produces textures the presenter cannot
    /// sample without a copy through system memory.
    MpResult open(const MpVideoCodecVtbl& vtbl, MpCodec codec,
                  const MpGraphicsDevice* device, const std::uint8_t* config,
                  std::uint32_t config_bytes);
    void close() noexcept;

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    MpResult decode(const void* packet, std::size_t bytes, std::uint64_t pts) noexcept;
    /// The next frame the decoder will part with, MP_END when it is holding
    /// none, or MP_ERR_BUSY when the stream changed and it wants asking again.
    MpResult next_frame(MpVideoFrame& out) noexcept;
    MpResult get_format(MpVideoInfo& out) noexcept;
    MpResult flush() noexcept;
    MpResult reset() noexcept;

private:
    const MpVideoCodecVtbl* vtbl_ = nullptr;
    MpVideoCodec* handle_ = nullptr;
};

/// A presenter module, behind the C vtable. `mp::Sink` for pictures.
class Presenter final {
public:
    Presenter() noexcept = default;
    ~Presenter() { close(); }

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;

    /// `window` is an HWND on Windows. **Null means off-screen**, which is not
    /// a degraded mode: it is how a presenter is measured.
    MpResult open(const MpVideoVtbl& vtbl, void* window);
    void close() noexcept;

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    MpResult configure(const MpVideoInfo& info) noexcept;
    MpResult present(const MpVideoFrame& frame) noexcept;
    MpResult set(const char* key, const char* value) noexcept;
    MpResult get_device(MpGraphicsDevice& out) noexcept;
    MpResult read_back(void* dst, std::size_t dst_bytes, std::uint32_t& width,
                       std::uint32_t& height, MpPixelLayout& layout) noexcept;

private:
    const MpVideoVtbl* vtbl_ = nullptr;
    MpVideo* handle_ = nullptr;
};

/// §8 applied: decode, and present when the audio clock says so.
class VideoGraph final {
public:
    /// `info` is the container's, which is what states the timescale and the
    /// frame rate; the decoder's own answer is read after the first frame and
    /// the presenter reconfigured if the bitstream disagrees.
    VideoGraph(IPacketFeed& packets, VideoDecoder& decoder, Presenter& presenter,
               const MpVideoInfo& info);

    VideoGraph(const VideoGraph&) = delete;
    VideoGraph& operator=(const VideoGraph&) = delete;
    VideoGraph(VideoGraph&&) = delete;
    VideoGraph& operator=(VideoGraph&&) = delete;

    /// How far the video timeline is from the audio one. See
    /// `VideoPacer::set_skew_seconds`, which is where the sixty milliseconds
    /// §9.9 measured has to be subtracted.
    void set_skew_seconds(double seconds) noexcept { pacer_.set_skew_seconds(seconds); }

    enum class Step {
        /// A frame was presented.
        shown,
        /// Nothing was due, or nothing was available. The picture already up
        /// stays up, which is the duplicate §8 describes and costs nothing to
        /// perform. **A feed answering MP_ERR_BUSY lands here too**: another
        /// consumer of the same demuxer has to read before this one can, and
        /// holding the picture is what to do meanwhile.
        repeated,
        /// A frame was past its time and was let go rather than shown late.
        dropped,
        /// The stream ended.
        finished,
        /// Something failed; `error()` says what.
        failed,
    };

    /// One decision, against the audio being heard at `audible_seconds`.
    ///
    /// Returns as soon as it has done one thing, so a caller driving it from a
    /// display loop calls it once per refresh. It may drop several frames on
    /// one call -- a decoder that fell behind has several past frames to let
    /// go, and letting them go one refresh at a time would never catch up.
    Step pump(double audible_seconds);

    struct Stats {
        /// Frames the presenter was given.
        std::uint64_t shown = 0;
        /// Frames let go because their time had passed.
        std::uint64_t dropped = 0;
        /// Frames the decoder produced. `shown + dropped` once the stream ends.
        std::uint64_t decoded = 0;
        /// The worst a shown frame was late by, in seconds. Never positive: a
        /// frame is not shown early.
        double worst_late_seconds = 0.0;
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }
    [[nodiscard]] MpResult error() const noexcept { return error_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// What the presenter is configured for -- the container's, until the
    /// decoder's first frame says otherwise.
    [[nodiscard]] const MpVideoInfo& info() const noexcept { return info_; }

private:
    /// Decodes until the decoder parts with a frame. MP_END when the stream is
    /// over and the decoder has been flushed and emptied.
    MpResult fetch();
    /// After the first frame: ask the decoder what it actually produced, and
    /// reconfigure the presenter if the bitstream disagrees with the container.
    void reconcile();

    IPacketFeed* packets_;
    VideoDecoder* decoder_;
    Presenter* presenter_;
    MpVideoInfo info_{};
    VideoPacer pacer_;

    MpVideoFrame frame_{};
    bool have_frame_ = false;
    bool reconciled_ = false;
    bool drained_ = false;
    bool finished_ = false;
    MpResult error_ = MP_OK;

    std::vector<std::uint8_t> buffer_;
    MpPacket packet_{};
    Stats stats_{};
};

} // namespace mp

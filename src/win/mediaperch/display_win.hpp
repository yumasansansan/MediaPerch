// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two clocks §8's loop needs, on Windows, and a window to draw into.
//
// `mp::DisplayLoop` decides what to do each turn and this decides when a turn
// happens. The split is the one the engine keeps everywhere: the policy is
// portable and testable, and the thing that blocks on hardware is not.

#include "mediaperch/display.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mp::win {

/// QueryPerformanceFrequency, once. It cannot change while the system runs.
[[nodiscard]] std::uint64_t qpc_rate() noexcept;
[[nodiscard]] std::uint64_t qpc_now() noexcept;

/// A frame clock that sleeps for a fixed period.
///
/// **The fallback, and not a bad one.** A flip-model swap chain presented with
/// a sync interval of zero does not tear -- DWM still shows the frame at a
/// vblank -- so a loop that wakes often enough presents smoothly without
/// knowing when the display refreshes. What it costs is wakeups: a period
/// short enough for a 240 Hz display is wasted on a 60 Hz one.
class TickClock final : public IFrameClock {
public:
    explicit TickClock(std::uint32_t period_us = 2000) noexcept : period_us_(period_us) {}

    bool wait() override;
    [[nodiscard]] std::uint64_t now() const override { return qpc_now(); }
    [[nodiscard]] std::uint64_t rate() const override { return qpc_rate(); }
    [[nodiscard]] double nominal_interval() const override
    {
        return static_cast<double>(period_us_) / 1e6;
    }

    /// Stops the loop that is waiting on it. Safe from another thread.
    void cancel() noexcept override { cancelled_ = true; }

private:
    std::uint32_t period_us_;
    volatile bool cancelled_ = false;
};

/// A frame clock that waits for the display the window is on.
///
/// `IDXGIOutput::WaitForVBlank`, on the output that actually contains the
/// window rather than the first one the adapter enumerates -- which on two
/// monitors at different rates is the difference between pacing to the right
/// display and pacing to a neighbour.
///
/// **It makes its own DXGI factory.** The presenter has one and does not hand
/// it out, and a second factory for timing costs a handful of kilobytes; the
/// alternative is an ABI addition to reach a swap chain, which is what
/// `IDXGISwapChain2::GetFrameLatencyWaitableObject` would want and is the
/// better pacing source the day something needs it.
class VBlankClock final : public IFrameClock {
public:
    ~VBlankClock() override;

    VBlankClock(const VBlankClock&) = delete;
    VBlankClock& operator=(const VBlankClock&) = delete;
    VBlankClock(VBlankClock&&) = delete;
    VBlankClock& operator=(VBlankClock&&) = delete;

    /// The output `window` is on, or null when there is none -- a headless
    /// session, or a window that has not been shown. A caller with no output
    /// uses `TickClock`.
    [[nodiscard]] static std::unique_ptr<VBlankClock> open(void* window);

    bool wait() override;
    [[nodiscard]] std::uint64_t now() const override { return qpc_now(); }
    [[nodiscard]] std::uint64_t rate() const override { return qpc_rate(); }
    [[nodiscard]] double nominal_interval() const override
    {
        return refresh_hz_ > 0.0 ? 1.0 / refresh_hz_ : 0.0;
    }

    void cancel() noexcept override { cancelled_ = true; }

    /// What the display says it refreshes at, for the log. Zero when it did
    /// not say.
    [[nodiscard]] double refresh_hz() const noexcept { return refresh_hz_; }

private:
    VBlankClock() = default;

    void* output_ = nullptr; ///< IDXGIOutput*, held without dragging DXGI in
    double refresh_hz_ = 0.0;
    volatile bool cancelled_ = false;
};

/// A window to draw a picture in.
///
/// **One process, one window** -- `MP_SURFACE_WINDOW` in §9.7.1's terms, which
/// is the case a tool has. The engine does not use this: §9.7.1 decided that a
/// headless engine which creates windows is not headless, and that the frame
/// crosses the process boundary as a DirectComposition surface instead. This is
/// for `mediaperch-probe`, which is one program looking at one file.

/// **The compositor's own event, as a frame clock** (§9.7.1).
///
/// An engine with no window has no vertical blank to wait on. A composition
/// swap chain asked for `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`
/// hands back an event the compositor sets when it will take another frame,
/// which is `WaitForVBlank`'s question answered by the thing that will actually
/// show the picture -- and it is the third `IFrameClock`, beside the vblank and
/// the tick, that §9.7.1 said this shape would produce.
///
/// **The handle is the presenter's and is not closed here.** It is made once
/// with the swap chain and closed with it; a clock that closed it would leave
/// the module reporting a handle that is not one.
/// **The compositor's own tick**, for §9.7.1's engine, which has no window and
/// so no vertical blank of its own to wait on.
///
/// `DCompositionWaitForCompositorClock` returns once per compositor frame,
/// whether or not this process presented anything. That distinction is the
/// whole reason this class exists, and getting it wrong cost a black window:
/// the swap chain's frame-latency waitable object looks like the same thing and
/// is not. **It is a throttle, not a heartbeat.** A waitable chain starts with
/// as many credits as its maximum frame latency, a wait takes one and a
/// `Present` gives one back -- so a loop that waits every turn and presents
/// only when a frame is due spends its credits on the turns that drew nothing
/// and then stops being signalled at all. Measured: one turn, one frame
/// decoded, nothing shown, and after that only the wait's own timeout, which is
/// a picture at one frame per second and looks exactly like no picture.
///
/// The frame-latency object still belongs where it always belonged: in front of
/// `Present`, which is the operation it throttles.
///
/// Resolved at run time rather than linked, for two reasons that point the same
/// way: `dcomp.h` does not compile under this tree's warning set without a
/// suppression, and the entry point is Windows 10 1809 and later, so a machine
/// without it should fall back rather than fail to start.
class CompositorClock final : public mp::IFrameClock {
public:
    /// Null when this Windows has no compositor clock to wait on. The caller
    /// then falls back, which is what `EngineHost::frame_clock` does.
    [[nodiscard]] static std::unique_ptr<CompositorClock> open();

    CompositorClock() = default;
    ~CompositorClock() override;

    CompositorClock(const CompositorClock&) = delete;
    CompositorClock& operator=(const CompositorClock&) = delete;
    CompositorClock(CompositorClock&&) = delete;
    CompositorClock& operator=(CompositorClock&&) = delete;

    bool wait() override;
    [[nodiscard]] std::uint64_t now() const override;
    [[nodiscard]] std::uint64_t rate() const override;
    void cancel() noexcept override;

    /// Zero: the compositor does not say what interval it will take frames at,
    /// and `DisplayLoop` measures the real one anyway.
    [[nodiscard]] double nominal_interval() const override { return 0.0; }

private:
    /// Passed to the wait alongside the compositor clock, so a stop is taken
    /// now rather than at the end of a refresh.
    void* stopping_ = nullptr;
    /// Whether the compositor last answered *occluded* rather than ticking;
    /// see `wait`. The loop's thread's alone.
    bool occluded_ = false;
    std::atomic<bool> cancelled_{false};
};

/// **The swap chain's frame-latency object, as a clock.** Kept as the fallback
/// for a Windows with no compositor clock, and it is a poor one for the reason
/// `CompositorClock` documents: it is signalled by presenting, so a loop that
/// skips a present waits out its own timeout. Better than nothing, which is
/// what the alternative is.
class WaitableClock final : public mp::IFrameClock {
public:
    explicit WaitableClock(void* waitable) noexcept : waitable_(waitable) {}

    bool wait() override;
    [[nodiscard]] std::uint64_t now() const override;
    [[nodiscard]] std::uint64_t rate() const override;
    void cancel() noexcept override { cancelled_ = true; }

    /// Zero: the compositor does not say what interval it will take frames at,
    /// and `DisplayLoop` measures the real one anyway -- which it does better
    /// than any mode's label, per §8's note on the estimator.
    [[nodiscard]] double nominal_interval() const override { return 0.0; }

private:
    void* waitable_;
    std::atomic<bool> cancelled_{false};
};

class VideoWindow final {
public:
    VideoWindow() = default;
    ~VideoWindow();

    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;
    VideoWindow(VideoWindow&&) = delete;
    VideoWindow& operator=(VideoWindow&&) = delete;

    /// `width` and `height` are the picture's; the window is made so its client
    /// area is that size, because a video window that is not the video's size
    /// scales on its first frame and looks like a bug.
    bool open(const std::string& title, std::uint32_t width, std::uint32_t height,
              std::string& why);

    /// The HWND, for `MpVideoVtbl::open`. Null until `open` succeeded.
    [[nodiscard]] void* handle() const noexcept { return window_; }

    /// The client area, in pixels.
    ///
    /// **What §9.7.1 hands to the engine.** The decision there was that the
    /// shell says a size and the engine renders at it, so the scale happens in
    /// our own shader beside the chroma reconstruction rather than in the
    /// compositor's bilinear -- and this is the number a shell sends. False
    /// before `open`, and false for a window that has been minimised, whose
    /// client area is nothing and whose size is not a size to render at.
    [[nodiscard]] bool client_size(std::uint32_t& width, std::uint32_t& height) const;

    /// Handles what has arrived without blocking. False once the window has
    /// been closed, which is a person saying stop.
    bool pump_messages();

    /// Whether somebody closed it.
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    void close() noexcept;

private:
    void* window_ = nullptr;
    bool closed_ = false;
};

} // namespace mp::win

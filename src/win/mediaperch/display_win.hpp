// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two clocks §8's loop needs, on Windows, and a window to draw into.
//
// `mp::DisplayLoop` decides what to do each turn and this decides when a turn
// happens. The split is the one the engine keeps everywhere: the policy is
// portable and testable, and the thing that blocks on hardware is not.

#include "mediaperch/display.hpp"

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
    void cancel() noexcept { cancelled_ = true; }

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

    void cancel() noexcept { cancelled_ = true; }

    /// What the display says it refreshes at, for the log. Zero when it did
    /// not say.
    [[nodiscard]] double refresh_hz() const noexcept { return refresh_hz_; }

private:
    VBlankClock() = default;

    void* output_ = nullptr; ///< IDXGIOutput*, held without dragging DXGI in
    double refresh_hz_ = 0.0;
    volatile bool cancelled_ = false;
};

/// The clock for a picture with no sound to follow.
///
/// **This is not §8's clock and does not pretend to be.** §8 says the audio
/// device is the master and everything follows it, and that is right whenever
/// there is one -- a crystal that is actually producing the sound somebody is
/// listening to. A file with no audio track has no such crystal, and a picture
/// still has to go up at some rate, so this counts the performance counter and
/// reports it as if it were a device playing at `rate`.
///
/// What it costs is what §8 was avoiding: the counter and a display are not the
/// same crystal either, so a long enough run drifts against the display. It
/// does not drift against anything a person can hear, because there is nothing
/// to hear. Whoever uses it should say so, and `mediaperch-probe show` does.
class WallClock final : public IAudioClockSource {
public:
    explicit WallClock(std::uint32_t rate = 48000) noexcept;

    [[nodiscard]] ClockSpec spec() const override { return spec_; }
    bool read(ClockReading& out) override;

    /// Starts counting. Before this it reports nothing, which is what keeps a
    /// picture from being drawn against a clock that has not begun.
    void start() noexcept;

private:
    ClockSpec spec_{};
    std::uint64_t origin_ = 0;
    bool running_ = false;
};

/// A window to draw a picture in.
///
/// **One process, one window** -- `MP_SURFACE_WINDOW` in §9.7.1's terms, which
/// is the case a tool has. The engine does not use this: §9.7.1 decided that a
/// headless engine which creates windows is not headless, and that the frame
/// crosses the process boundary as a DirectComposition surface instead. This is
/// for `mediaperch-probe`, which is one program looking at one file.
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

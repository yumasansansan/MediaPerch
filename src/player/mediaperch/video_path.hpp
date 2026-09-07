// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The picture, assembled -- and assembled where the second shell can reach it.
//
// **All of this was inside `show`.** One command in the Windows head opened a
// presenter, chose a decoder, handed it the presenter's device, built a
// `VideoGraph` and ran a `DisplayLoop` on a thread of its own. Every line of
// that is portable and none of it was reachable from anywhere else, so the
// engine of §9.7.1 -- which has no window and still needs all of it -- would
// have had to grow a second copy, and so would the Linux head, and so would
// anything that embeds this. It lives here now, and the head keeps what
// genuinely is a window: the HWND, the message pump, the mode switch, and the
// report it prints.
//
// **It owns the loop's thread, and not the clocks.** `DisplayLoop` holds no
// thread on purpose -- which thread pumps a window's messages is the head's
// business -- but that argument is about the *head's* thread, not about the
// loop's, and every caller so far has written the same std::thread, cancel,
// join sequence by hand. So the thread is here. The two clocks stay the
// caller's: a vblank where there is a display, a tick where there is not, and
// whichever audio graph is playing.
//
// **What it is not.** It does not read the file: §4 gives a file one position
// and `PacketRouter` owns it, so this takes a feed and never a demuxer. It
// does not seek: that is `mp::seek_together`, which needs the audio graph as
// well and is one move rather than two. And it opens nothing itself -- the two
// doors it goes through are `IEngineHost`'s, which is what keeps LoadLibrary
// and D3D out of here.

#ifndef MEDIAPERCH_VIDEO_PATH_HPP
#define MEDIAPERCH_VIDEO_PATH_HPP

#include "mediaperch/display.hpp"
#include "mediaperch/packet.hpp"
#include "mediaperch/video.hpp"

#include <mediaperch/module.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace mp {

class IEngineHost;

class VideoPath final {
public:
    /// What a caller decides, as against what the file says.
    struct Config {
        /// Zero leaves it to the decoder, which is what the ABI's default is.
        /// See `mp::ThreadChoice`: what the number *means* is the module's.
        std::uint32_t decoder_threads = 0;

        /// **§9.7.1: the size the picture is rendered at.** Zero is the
        /// picture's own, which is what a run with nothing to compose into
        /// wants. A shell says otherwise, and says it again when its window
        /// changes -- see `set_size`.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    /// What it turned out to be, for whoever reports the run.
    struct Modules {
        std::string presenter;
        std::string decoder;
    };

    VideoPath() noexcept = default;
    ~VideoPath();

    VideoPath(const VideoPath&) = delete;
    VideoPath& operator=(const VideoPath&) = delete;
    VideoPath(VideoPath&&) = delete;
    VideoPath& operator=(VideoPath&&) = delete;

    /// Opens a presenter, hands its device to a decoder, and builds the graph.
    ///
    /// `window` is the head's own and is opaque here -- an HWND on Windows.
    /// **Null is the engine's case and not a degraded one**: §9.7.1 has a
    /// windowless engine draw into a composition surface a shell composites,
    /// and only a tool that owns a window passes one.
    ///
    /// False and a reason when nothing here presents, nothing decodes, or the
    /// presenter will not take the picture. Nothing is left half-open.
    bool open(IEngineHost& host, void* window, IPacketFeed& feed,
              const MpVideoInfo& picture, MpCodec codec, const std::uint8_t* config,
              std::uint32_t config_bytes, const Config& want, std::string& why);

    [[nodiscard]] bool opened() const noexcept { return graph_ != nullptr; }

    /// Starts the display loop on a thread of its own, on the clock the
    /// presenter brought with it.
    ///
    /// **Not this thread**, because `IFrameClock::wait` blocks for a whole
    /// refresh and a head that waited in it would be a window Windows calls
    /// unresponsive.
    ///
    /// False when the presenter brought none -- off-screen, or a display that
    /// would not answer. The caller then supplies one or does not draw.
    bool start(IAudioClockSource& audio, std::string& why);

    /// The same, on a clock the caller has instead. `show` uses it for the
    /// tick fallback a machine with no vertical blank gets.
    bool start(IAudioClockSource& audio, IFrameClock& frames, std::string& why);

    /// The clock the presenter brought, or null. Handed out so a report can
    /// say what is pacing the picture, which is a thing worth printing.
    [[nodiscard]] IFrameClock* clock() noexcept { return own_frames_.get(); }

    /// Cancels the frame clock and joins. Safe twice, and safe unopened.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    /// Whether the loop finished by itself -- the stream ran out, or the
    /// display went away. False while it is still turning.
    [[nodiscard]] bool ended() const noexcept
    {
        return ended_.load(std::memory_order_acquire);
    }

    /// **§9.7.1's message, arriving.** The size to render at; zero for the
    /// picture's own.
    ///
    /// Safe while the loop is turning, and it has to be: a window dragged by a
    /// corner is this call arriving forty times a second. A resize replaces
    /// the swap chain's buffers and a graphics context is not two threads' to
    /// share, so this takes the same hold on the loop that `seek_together`
    /// does, for the same reason, and waits to be told the loop has parked
    /// rather than assuming it has.
    bool set_size(std::uint32_t width, std::uint32_t height, std::string& why,
                  std::chrono::milliseconds deadline = std::chrono::milliseconds{500});

    [[nodiscard]] const Modules& modules() const noexcept { return modules_; }

    /// The three pieces, for the caller that reports the run and for
    /// `seek_together`, which needs the graph and the loop and the audio.
    ///
    /// Handed out rather than wrapped: a `stats()` here would be a third
    /// spelling of numbers `VideoGraph` and `DisplayLoop` already publish, and
    /// two answers to one question are two things to keep in step.
    [[nodiscard]] Presenter& presenter() noexcept { return *presenter_; }
    [[nodiscard]] VideoGraph& graph() noexcept { return *graph_; }
    [[nodiscard]] DisplayLoop& loop() noexcept { return *loop_; }

private:
    /// One `size` at the presenter, with the value spelled the way §9.7.1's
    /// setting takes it.
    bool tell_size(std::uint32_t width, std::uint32_t height, std::string& why);

    std::unique_ptr<Presenter> presenter_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<VideoGraph> graph_;
    std::unique_ptr<DisplayLoop> loop_;
    /// **The presenter's own**, fetched once `configure` has made the swap
    /// chain there is an event on. Owned here because the presenter is owned
    /// here and the two go together.
    std::unique_ptr<IFrameClock> own_frames_;
    Modules modules_;

    IFrameClock* frames_ = nullptr;
    std::thread thread_;
    std::atomic<bool> ended_{false};
};

} // namespace mp

#endif // MEDIAPERCH_VIDEO_PATH_HPP

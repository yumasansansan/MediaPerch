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
// doors it goes through are `IVideoHost`'s, which is what keeps LoadLibrary
// and D3D out of here.

#ifndef MEDIAPERCH_VIDEO_PATH_HPP
#define MEDIAPERCH_VIDEO_PATH_HPP

#include "mediaperch/clock.hpp"
#include "mediaperch/display.hpp"
#include "mediaperch/packet.hpp"
#include "mediaperch/video.hpp"
#include "mediaperch/video_host.hpp"

#include <mediaperch/module.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <thread>

namespace mp {

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

        /// **§9.8.3's chain**, as `name` or `name:key=value,key=value` in the
        /// order it runs -- the same grammar the audio chain's `--dsp` uses,
        /// because two grammars for one idea would be one too many.
        std::vector<std::string> stages;
    };

    /// **What the shell says the display is** (§9.4, §9.7.1).
    ///
    /// The one thing a windowless engine cannot work out for itself: given no
    /// window, a presenter falls back to the first output, and for an engine
    /// that is not a fallback but a guess about which monitor the picture is
    /// on. Every §9 decision turns on it -- the tone mapper, the SDR boost, the
    /// HLG system gamma, the encoding.
    struct DisplayIs {
        bool hdr = false;
        /// Advanced Color on with an SDR mode: a wide-gamut display managing
        /// its own colour. Only distinguishable from plain SDR on Windows 11
        /// 24H2 and later, so a shell that cannot tell leaves it false.
        bool wide = false;
        /// `DISPLAYCONFIG_SDR_WHITE_LEVEL` in nits. 80 is scRGB's reference and
        /// what a display that does not say is assumed to use.
        float white_nits = 80.0f;
        /// The brightest it claims. **HLG needs it and PQ does not**: PQ states
        /// absolute nits, HLG's OOTF has a system gamma that is a function of
        /// this. 1000 is BT.2100's reference display.
        float peak_nits = 1000.0f;
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
    bool open(IVideoHost& host, void* window, IPacketFeed& feed,
              const MpVideoInfo& picture, MpCodec codec, const std::uint8_t* config,
              std::uint32_t config_bytes, const Config& want, std::string& why);

    [[nodiscard]] bool opened() const noexcept { return graph_ != nullptr; }

    /// **The next track, on the presenter that is already open.**
    ///
    /// A boundary changes the file, not the display. §8 does not rebuild the
    /// audio device when a track ends -- not rebuilding it is what gapless
    /// *is* -- and the picture had no equivalent: it was torn down and built
    /// again, presenter and all, for every track. Nothing looked wrong until
    /// there was a shell, because the presenter is where the composition
    /// surface lives: a new presenter is a new handle, so the shell had to
    /// detach and attach, and on a playlist of short files that is a black
    /// frame and then an empty one, once a second.
    ///
    /// So the presenter stays, with its surface, its chain, its size and the
    /// display it was told about; the decoder is opened again because the codec
    /// may have changed, the graph is rebuilt around the new feed, and the
    /// presenter is configured for the new picture. The frame clock is fetched
    /// again because `configure` may replace the swap chain and the waitable
    /// belongs to the chain.
    ///
    /// False and a reason when this track cannot go on this presenter -- a
    /// codec nothing decodes, a picture it will not take. The caller then opens
    /// the whole path afresh, which is what `open` is for.
    bool retrack(IVideoHost& host, IPacketFeed& feed, const MpVideoInfo& picture,
                 MpCodec codec, const std::uint8_t* config, std::uint32_t config_bytes,
                 const Config& want, std::string& why);

    /// Starts the display loop on a thread of its own, on the clock the
    /// presenter brought with it, following `follow`.
    ///
    /// **Null is a picture on its own.** With nothing to follow, the loop
    /// runs on the video engine's own clock -- a `FreeClock` over the frame
    /// clock's counter, started with the loop -- which is what a file with no
    /// audio plays on, and what `own_clock()` then hands a transport. With a
    /// clock to follow, the picture is synchronised to it and the engine's own
    /// is not made: one clock leads, and it is the caller's to say which.
    ///
    /// **Not this thread**, because `IFrameClock::wait` blocks for a whole
    /// refresh and a head that waited in it would be a window Windows calls
    /// unresponsive.
    ///
    /// False when the presenter brought no clock -- off-screen, or a display
    /// that would not answer. The caller then supplies one or does not draw.
    /// `origin_seconds` is where this track began on the clock being followed,
    /// which is zero for anything playing a file on its own and is not for a
    /// queue. See `DisplayLoop`'s constructor for why the two coordinates
    /// differ.
    bool start(IMediaClock* follow, std::string& why, double origin_seconds = 0.0);

    /// The same, on a frame clock the caller has instead. `show` uses it for
    /// the tick fallback a machine with no vertical blank gets.
    bool start(IMediaClock* follow, IFrameClock& frames, std::string& why,
               double origin_seconds = 0.0);

    /// What the engine's own timeline counts in when a picture plays alone:
    /// milliseconds. The same for every file, so a shell can count in it, and
    /// no finer than a container's timestamps need.
    static constexpr std::uint32_t k_own_rate = 1000;

    /// The engine's own clock, while the loop runs on it; null while it
    /// follows somebody else's, and before `start`. A transport pauses,
    /// resumes and reads the position through this.
    [[nodiscard]] FreeClock* own_clock() noexcept { return own_clock_.get(); }

    /// A seek for a picture running on its own clock: holds the loop, moves
    /// the file through `move` (in seconds), rewinds the graph and re-anchors
    /// the clock. `mp::seek_together` is the same move with an audio graph in
    /// the middle; this is what is left of it when there is none.
    bool seek_alone(double seconds, const std::function<bool(double)>& move, std::string& why,
                    std::chrono::milliseconds deadline = std::chrono::milliseconds{500});

    /// The clock the presenter brought, or null. Handed out so a report can
    /// say what is pacing the picture, which is a thing worth printing.
    [[nodiscard]] IFrameClock* clock() noexcept { return own_frames_.get(); }

    /// **The composition surface, as the number §9.7.1 says it is.** Zero for a
    /// presenter drawing into a window, or one that has not been configured.
    ///
    /// Read off the settings surface rather than through an entry point of its
    /// own: the handle has to cross a process boundary and `describe` is where
    /// a presenter prints one, so reading it back here is the same conversation
    /// with one fewer process in it.
    [[nodiscard]] std::uint64_t surface() noexcept;

    /// The presenter's own `describe` rows, as raw `key\tvalue\tdescription`
    /// lines. Gated (see `gate_`); no loop hold, because the presenter's
    /// describe reads status and touches no graphics context.
    [[nodiscard]] std::vector<std::string> presenter_describe();

    /// The graph's counters and the loop's, copied out under the gate. Zeroed
    /// when there is no graph or the loop is not turning. **What a shell reads
    /// to tell a connected picture from a drawn one**, and it has to be gated
    /// because a track boundary resets both underneath it.
    [[nodiscard]] VideoGraph::Stats graph_stats() const;
    [[nodiscard]] DisplayLoop::Stats loop_stats() const;

    /// Cancels the frame clock and joins. Safe twice, and safe unopened.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    /// Whether the loop finished by itself -- the stream ran out, or the
    /// display went away. False while it is still turning.
    [[nodiscard]] bool ended() const noexcept
    {
        return ended_.load(std::memory_order_acquire);
    }

    /// One setting straight at the presenter, taking the display loop's hold.
    ///
    /// **What a shell reaches through.** `size` and `display` have their own
    /// calls below because the engine sends them itself as well; this is every
    /// other key the presenter has -- the tone mapper, whether anything is
    /// composited over the video -- which only a person ever changes.
    bool set_presenter(const std::string& key, const std::string& value, std::string& why,
                       std::chrono::milliseconds deadline = std::chrono::milliseconds{500})
    {
        return tell(key.c_str(), value.c_str(), why, deadline);
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

    /// **§9.7.1's other message.** Which display the picture is on, and what it
    /// is. Sent when a shell's window crosses a monitor or somebody toggles
    /// HDR, and once when the file opens.
    ///
    /// More expensive than a resize and rarer: the whole colour plan is decided
    /// again and the swap chain rebuilt from its answer, because the format and
    /// the colour space may both have changed. Takes the same hold on the loop,
    /// for the same reason.
    bool set_display(const DisplayIs& display, std::string& why,
                     std::chrono::milliseconds deadline = std::chrono::milliseconds{500});

    /// Back to the presenter working it out for itself, which is what a window
    /// this process owns wants and what an off-screen measurement needs.
    bool probe_display(std::string& why,
                       std::chrono::milliseconds deadline = std::chrono::milliseconds{500});

    [[nodiscard]] const Modules& modules() const noexcept { return modules_; }

    /// How many stages are in the chain, and what one of them is called. A
    /// canvas asks by index because that is the order they run in, which is the
    /// one thing a chain has that a set does not.
    [[nodiscard]] std::size_t stage_count() const noexcept { return stages_.size(); }
    [[nodiscard]] const std::string& stage_module(std::size_t index) const noexcept;

    /// One stage's own settings, and changing one.
    ///
    /// **Both take the display loop's hold**, for the reason `set_size` does: a
    /// stage is a module the loop's thread is inside every frame, and asking it
    /// anything from another thread while it is there is a data race rather
    /// than a question.
    [[nodiscard]] std::vector<std::string> stage_describe(
        std::size_t index,
        std::chrono::milliseconds deadline = std::chrono::milliseconds{500});
    bool set_stage(std::size_t index, const std::string& key, const std::string& value,
                   std::string& why,
                   std::chrono::milliseconds deadline = std::chrono::milliseconds{500});

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
    /// One setting at the presenter, taking the loop's hold if it is turning.
    ///
    /// **Every message that changes what the presenter is doing goes through
    /// here.** A resize replaces the swap chain's buffers and a display change
    /// replaces the chain; a graphics context is not two threads' to share, and
    /// one place to take the hold is one place to get it right.
    bool tell(const char* key, const char* value, std::string& why,
              std::chrono::milliseconds deadline);

    /// Opens the chain from `want.stages` and hands it to the presenter.
    /// Never fatal: a stage that will not open is a stage the run says it is
    /// without, and the picture is still a picture.
    void open_stages(IVideoHost& host, const Config& want);
    /// Everything the presenter must be told again after the chain changed.
    bool hand_over(std::string& why);

    std::unique_ptr<Presenter> presenter_;
    std::vector<std::unique_ptr<VideoStage>> stages_;
    std::vector<std::string> stage_modules_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<VideoGraph> graph_;
    std::unique_ptr<DisplayLoop> loop_;
    /// **The presenter's own**, fetched once `configure` has made the swap
    /// chain there is an event on. Owned here because the presenter is owned
    /// here and the two go together.
    std::unique_ptr<IFrameClock> own_frames_;
    /// The engine's own timeline, made by `start` when there is nothing to
    /// follow. Declared after `own_frames_`, whose counter it reads, so it is
    /// destroyed first.
    std::unique_ptr<FreeClock> own_clock_;
    Modules modules_;

    /// The head's window, or null for §9.7.1's engine. Kept because the frame
    /// clock is asked for again at every `retrack` and the question includes it.
    void* window_ = nullptr;

    IFrameClock* frames_ = nullptr;
    std::thread thread_;
    std::atomic<bool> ended_{false};

    /// **One thread at a time inside the presenter.** The picture is rebuilt on
    /// the engine thread -- `open`, `retrack`, `start`, `stop` at a track
    /// boundary -- and reached on IPC threads that resize it, name a display,
    /// set a stage or ask what it is doing. `Player::picture()` hands out a
    /// `shared_ptr`, which keeps the object alive across such a call but does
    /// nothing to stop the two threads being inside the *same D3D11 immediate
    /// context* at once, which is neither thread-safe nor forgiving: a resize
    /// arriving while a boundary rebuilt the decoder corrupted the heap and
    /// took the engine with it (`0xc0000374`), reproducible on the second
    /// resize of a running queue.
    ///
    /// So every entry that reads or rebuilds the presenter, the graph or the
    /// chain takes this first. **The display loop does not**: it runs on
    /// `thread_` from references handed to it at `start`, calls back into
    /// nothing here, and is excluded from a rebuild by `stop` joining it and
    /// from a setting by the loop's own hold. So the gate serialises only the
    /// engine thread against the IPC threads, and never the per-frame path.
    ///
    /// Recursive because a rebuild nests: `open` and `retrack` call `stop`.
    mutable std::recursive_mutex gate_;
};

} // namespace mp

#endif // MEDIAPERCH_VIDEO_PATH_HPP

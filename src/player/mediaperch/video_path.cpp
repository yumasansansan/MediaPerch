// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/video_path.hpp"

#include "mediaperch/player.hpp"
#include "mediaperch/result.hpp"

#include <cstdio>

namespace mp {

VideoPath::~VideoPath()
{
    stop();
}

bool VideoPath::open(IEngineHost& host, void* window, IPacketFeed& feed,
                     const MpVideoInfo& picture, MpCodec codec,
                     const std::uint8_t* config, std::uint32_t config_bytes,
                     const Config& want, std::string& why)
{
    stop();
    graph_.reset();
    own_frames_.reset();
    decoder_.reset();
    presenter_.reset();
    modules_ = Modules{};

    // **The presenter first, because the decoder wants its device** (§9.8.1).
    // A decoder that makes its own D3D11 device produces textures this
    // presenter cannot sample without a copy through system memory, which is
    // the whole of why the order is this way round and not the other.
    std::unique_ptr<Presenter> presenter = host.open_presenter(window, modules_.presenter, why);
    if (presenter == nullptr) {
        return false;
    }

    // Before `configure`, which is where the target is made.
    if (want.width != 0 || want.height != 0) {
        char value[32];
        std::snprintf(value, sizeof value, "%ux%u", want.width, want.height);
        const MpResult told = presenter->set("size", value);
        if (told != MP_OK) {
            why = std::string{"the presenter would not render at "} + value + ": " +
                  result_name(told);
            return false;
        }
    }
    if (presenter->configure(picture) != MP_OK) {
        why = "the presenter would not take that picture";
        return false;
    }

    MpGraphicsDevice device{};
    const bool have_device = presenter->get_device(device) == MP_OK;

    std::unique_ptr<VideoDecoder> decoder = host.open_video_decoder(
        codec, have_device ? &device : nullptr, config, config_bytes, modules_.decoder, why);
    if (decoder == nullptr) {
        return false;
    }
    if (want.decoder_threads != 0) {
        // **Between `open` and the first packet**, which is where a decoder
        // that starts a worker pool can still be told how big to make it.
        const std::string count = std::to_string(want.decoder_threads);
        const MpResult told = decoder->set("threads", count.c_str());
        if (told != MP_OK) {
            why = modules_.decoder + " would not take " + count + " threads: " +
                  result_name(told);
            return false;
        }
    }

    presenter_ = std::move(presenter);
    decoder_ = std::move(decoder);
    graph_ = std::make_unique<VideoGraph>(feed, *decoder_, *presenter_, picture);
    // **After `configure`, which is where the chain the event belongs to is
    // made.** Null is a real answer and not a failure: an off-screen presenter
    // has nothing that will show what it draws, so nothing that says when.
    own_frames_ = host.frame_clock(*presenter_, window);
    return true;
}

bool VideoPath::start(IAudioClockSource& audio, std::string& why)
{
    if (own_frames_ == nullptr) {
        why = "this presenter has no clock to pace on";
        return false;
    }
    return start(audio, *own_frames_, why);
}

bool VideoPath::start(IAudioClockSource& audio, IFrameClock& frames, std::string& why)
{
    if (graph_ == nullptr) {
        why = "there is no video graph to run";
        return false;
    }
    if (running()) {
        why = "the display loop is already turning";
        return false;
    }
    ended_.store(false, std::memory_order_release);
    frames_ = &frames;
    loop_ = std::make_unique<DisplayLoop>(*graph_, audio, frames);
    thread_ = std::thread{[this] {
        loop_->run();
        ended_.store(true, std::memory_order_release);
    }};
    return true;
}

void VideoPath::stop() noexcept
{
    if (!thread_.joinable()) {
        frames_ = nullptr;
        return;
    }
    // **The clock, not the loop.** `run` returns when a turn says stop, and a
    // turn is inside `wait` for a whole refresh; telling the clock is what
    // makes that return now rather than in sixteen milliseconds. A hold would
    // not do it -- a held loop keeps turning, which is the point of a hold.
    if (frames_ != nullptr) {
        frames_->cancel();
    }
    thread_.join();
    frames_ = nullptr;
}

bool VideoPath::tell(const char* key, const char* value, std::string& why,
                     std::chrono::milliseconds deadline)
{
    if (presenter_ == nullptr) {
        why = "there is no presenter to tell";
        return false;
    }
    const auto say = [&] {
        const MpResult told = presenter_->set(key, value);
        if (told != MP_OK) {
            why = std::string{"the presenter would not take "} + key + " = " + value + ": " +
                  result_name(told);
            return false;
        }
        return true;
    };
    if (!running()) {
        return say();
    }

    loop_->hold();
    // The same wait `seek_together` takes, and for the same reason it takes a
    // deadline: a loop whose display has gone away has no turn in which to
    // answer, and refusing the message because nobody is drawing would be
    // refusing for the wrong reason.
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool ok = say();
    loop_->release();
    return ok;
}

bool VideoPath::set_size(std::uint32_t width, std::uint32_t height, std::string& why,
                         std::chrono::milliseconds deadline)
{
    char value[32];
    if (width == 0 || height == 0) {
        std::snprintf(value, sizeof value, "native");
    } else {
        std::snprintf(value, sizeof value, "%ux%u", width, height);
    }
    return tell("size", value, why, deadline);
}

bool VideoPath::set_display(const DisplayIs& display, std::string& why,
                            std::chrono::milliseconds deadline)
{
    // Every field, every time. A message that carried only what changed would
    // put the presenter's idea of the display and the shell's out of step the
    // first time one was dropped, and there is nothing to gain: this is sent
    // when a window crosses a monitor, which is rare.
    char value[128];
    std::snprintf(value, sizeof value, "hdr=%d,wide=%d,white=%.4f,peak=%.4f",
                  display.hdr ? 1 : 0, display.wide ? 1 : 0,
                  static_cast<double>(display.white_nits),
                  static_cast<double>(display.peak_nits));
    return tell("display", value, why, deadline);
}

bool VideoPath::probe_display(std::string& why, std::chrono::milliseconds deadline)
{
    return tell("display", "probe", why, deadline);
}

} // namespace mp

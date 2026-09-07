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
    return true;
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

bool VideoPath::tell_size(std::uint32_t width, std::uint32_t height, std::string& why)
{
    char value[32];
    if (width == 0 || height == 0) {
        std::snprintf(value, sizeof value, "native");
    } else {
        std::snprintf(value, sizeof value, "%ux%u", width, height);
    }
    const MpResult told = presenter_->set("size", value);
    if (told != MP_OK) {
        why = std::string{"the presenter would not render at "} + value + ": " +
              result_name(told);
        return false;
    }
    return true;
}

bool VideoPath::set_size(std::uint32_t width, std::uint32_t height, std::string& why,
                         std::chrono::milliseconds deadline)
{
    if (presenter_ == nullptr) {
        why = "there is no presenter to resize";
        return false;
    }
    if (!running()) {
        return tell_size(width, height, why);
    }

    loop_->hold();
    // The same wait `seek_together` takes, and for the same reason it takes a
    // deadline: a loop whose display has gone away has no turn in which to
    // answer, and refusing to resize because nobody is drawing would be
    // refusing for the wrong reason.
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool ok = tell_size(width, height, why);
    loop_->release();
    return ok;
}

} // namespace mp

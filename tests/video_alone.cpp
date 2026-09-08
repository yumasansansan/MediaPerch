// SPDX-License-Identifier: GPL-3.0-or-later
//
// **The video engine, and nothing else.** This program links MediaPerch::video
// and MediaPerch::core and no audio library; if the video engine ever came to
// need a symbol of the audio one, this is where the link would fail, and
// nowhere else. It implements the video engine's own door to the platform
// over the fakes, opens a path, runs it on the engine's own clock for fifty
// turns, and checks that frames went up.

#include "fake_video.hpp"

#include "mediaperch/clock.hpp"
#include "mediaperch/display.hpp"
#include "mediaperch/video.hpp"
#include "mediaperch/video_host.hpp"
#include "mediaperch/video_path.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/// A packet whenever asked, forty milliseconds after the last.
class Feed final : public mp::IPacketFeed {
public:
    MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) override
    {
        buffer.assign(16, 0x5A);
        out = MpPacket{};
        out.size = sizeof(out);
        out.bytes = static_cast<std::uint32_t>(buffer.size());
        out.frame = frame_;
        frame_ += 40;
        return MP_OK;
    }

private:
    std::uint64_t frame_ = 0;
};

/// A display that refreshes fifty times, a sixtieth of a second apart.
class CountedFrames final : public mp::IFrameClock {
public:
    bool wait() override
    {
        if (cancelled_.load(std::memory_order_acquire) || left_ == 0) {
            return false;
        }
        --left_;
        now_ += 166'667;
        return true;
    }
    [[nodiscard]] double nominal_interval() const override { return 1.0 / 60.0; }
    [[nodiscard]] std::uint64_t now() const override { return now_; }
    [[nodiscard]] std::uint64_t rate() const override { return 10'000'000; }
    void cancel() noexcept override { cancelled_.store(true, std::memory_order_release); }

private:
    std::uint64_t left_ = 50;
    std::uint64_t now_ = 0;
    std::atomic<bool> cancelled_{false};
};

/// What a product with a picture and no sound implements: four doors.
class Host final : public mp::IVideoHost {
public:
    std::unique_ptr<mp::Presenter> open_presenter(void* window, std::string& module,
                                                  std::string& why) override
    {
        auto presenter = std::make_unique<mp::Presenter>();
        if (presenter->open(mp::test::fake_presenter_vtbl(), window) != MP_OK) {
            why = "the fake presenter would not open";
            return nullptr;
        }
        module = "video_test";
        return presenter;
    }
    std::unique_ptr<mp::IFrameClock> frame_clock(mp::Presenter&, void*) override
    {
        return std::make_unique<CountedFrames>();
    }
    std::unique_ptr<mp::VideoDecoder> open_video_decoder(MpCodec codec,
                                                         const MpGraphicsDevice* device,
                                                         const std::uint8_t* config,
                                                         std::uint32_t config_bytes,
                                                         std::string& module,
                                                         std::string& why) override
    {
        auto decoder = std::make_unique<mp::VideoDecoder>();
        if (decoder->open(mp::test::fake_video_codec_vtbl(), codec, device, config,
                          config_bytes) != MP_OK) {
            why = "the fake decoder would not open";
            return nullptr;
        }
        module = "vcodec_test";
        return decoder;
    }
};

} // namespace

int main()
{
    mp::test::presenter_log().reset();
    mp::test::decoder_log().reset();
    {
        const std::lock_guard lock{mp::test::decoder_log().mutex};
        mp::test::decoder_log().frames = 10'000;
    }

    MpVideoInfo picture{};
    picture.size = sizeof(picture);
    picture.width = 64;
    picture.height = 48;
    picture.display_width = 64;
    picture.display_height = 48;
    picture.timescale = 1000;
    picture.fps_num = 25;
    picture.fps_den = 1;

    Host host;
    Feed feed;
    mp::VideoPath path;
    std::string why;
    if (!path.open(host, nullptr, feed, picture, MP_CODEC_AV1, nullptr, 0, {}, why)) {
        std::fprintf(stderr, "video alone: %s\n", why.c_str());
        return 1;
    }
    // Nothing to follow: the engine's own clock, over the frame clock above.
    if (!path.start(nullptr, why)) {
        std::fprintf(stderr, "video alone: %s\n", why.c_str());
        return 1;
    }
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!path.ended() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    path.stop();

    const mp::VideoGraph::Stats graph = path.graph_stats();
    // The loop's own counters, not `loop_stats()`, which is zeroed once the
    // loop has stopped turning.
    const mp::DisplayLoop::Stats loop = path.loop().stats();
    const std::uint64_t at = path.own_clock() != nullptr ? path.own_clock()->position() : 0;
    std::printf("video alone: %llu turns, %llu without a clock, %llu shown, %llu dropped, "
                "clock at %llu ms\n",
                static_cast<unsigned long long>(loop.turns),
                static_cast<unsigned long long>(loop.without_clock),
                static_cast<unsigned long long>(graph.shown),
                static_cast<unsigned long long>(graph.dropped),
                static_cast<unsigned long long>(at));
    return loop.turns == 50 && loop.without_clock == 0 && graph.shown > 0 && at >= 800 &&
                   at <= 900
               ? 0
               : 1;
}

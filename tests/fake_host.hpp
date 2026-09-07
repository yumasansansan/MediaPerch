// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an operating system looks like to `mp::Player`.
//
// That is the useful thing this file says: `IEngineHost` is the whole of what
// the engine asks a platform for, so a platform a test invents is enough to run
// the entire product with no COM, no LoadLibrary, no audio hardware and no
// display. If this file ever grows, the split between the core and the head has
// moved and somebody should ask why.
//
// **It grew once, and the answer was good.** §9.7.1's video path moved into
// `src/player`, so the engine now asks for a presenter and a video decoder as
// well; the counters that answer those two are in fake_video.hpp, beside the
// audio ones here, and `mp::VideoPath` is what does the assembling. What did
// *not* happen is any of the assembly moving into a host.

#ifndef MEDIAPERCH_TESTS_FAKE_HOST_HPP
#define MEDIAPERCH_TESTS_FAKE_HOST_HPP

#include "fake_sink.hpp"
#include "fake_video.hpp"

#include "mediaperch/player.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mp::test {

inline Format cd_audio()
{
    return Format{.sample_rate = 44100,
                  .channels = 2,
                  .channel_mask = 0,
                  .sample_type = SampleType::s16,
                  .encoding = Encoding::pcm,
                  .valid_bits = 0};
}

/// Bytes a test chose, seekable, with an end.
class Tape final : public ISource {
public:
    Tape(const Format& format, std::vector<std::uint8_t> data)
        : format_(format), data_(std::move(data))
    {
    }

    [[nodiscard]] const Format& format() const noexcept override { return format_; }

    std::size_t read(void* dst, std::size_t bytes) override
    {
        const std::size_t stride = frame_bytes(format_);
        const std::size_t n = std::min(bytes - (bytes % stride), data_.size() - at_);
        std::memcpy(dst, data_.data() + at_, n);
        at_ += n;
        return n;
    }

    [[nodiscard]] bool seekable() const noexcept override { return true; }
    bool seek(std::uint64_t frame) override
    {
        const std::size_t offset = static_cast<std::size_t>(frame) * frame_bytes(format_);
        if (offset > data_.size()) {
            return false;
        }
        at_ = offset;
        return true;
    }
    [[nodiscard]] std::uint64_t length_frames() const noexcept override
    {
        return data_.size() / frame_bytes(format_);
    }

private:
    Format format_;
    std::vector<std::uint8_t> data_;
    std::size_t at_ = 0;
};

/// Distinguishable bytes: a ramp would survive an off-by-one shift unnoticed.
inline std::vector<std::uint8_t> pattern(std::size_t bytes, std::uint8_t seed)
{
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37u + seed * 101u + 11u) & 0xFFu);
    }
    return out;
}

/// A file, as `IEngineHost` hands one back: bytes for the audio, and a video
/// feed when the test asked for one.
///
/// **The two share nothing here and that is a fake's licence.** A real one is a
/// demuxer, a router and one position (§4); what a `Player` test is checking is
/// that the picture is opened, started against the audio clock, and stopped --
/// not that a router routes, which is packet_test.cpp's.
class TapeMedia final : public IMedia {
public:
    TapeMedia(const Format& format, std::vector<std::uint8_t> data, bool with_video)
        : audio_(format, std::move(data)), with_video_(with_video)
    {
    }

    [[nodiscard]] ISource& audio() noexcept override { return audio_; }
    [[nodiscard]] IPacketFeed* video() noexcept override
    {
        return with_video_ ? &feed_ : nullptr;
    }
    [[nodiscard]] Picture picture() const noexcept override
    {
        Picture out;
        out.info.size = sizeof(out.info);
        out.info.width = 16;
        out.info.height = 16;
        out.info.display_width = 16;
        out.info.display_height = 16;
        out.info.timescale = 1000;
        out.info.fps_num = 25;
        out.info.fps_den = 1;
        out.codec = MP_CODEC_AV1;
        return out;
    }
    [[nodiscard]] const std::string& decoder() const noexcept override { return by_; }

private:
    /// A packet whenever asked, because what the audio does is the subject and
    /// a feed that ran out would end the picture before the track did.
    class Endless final : public IPacketFeed {
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

    Tape audio_;
    Endless feed_;
    bool with_video_;
    std::string by_{"decode_test"};
};

/// A name is a file, a device is the fake sink, and a log line is a string.
class Host final : public IEngineHost {
public:
    Host()
    {
        rules_.period_frames = 64;
        // A device with a clock. An engine is a claim about a stream that keeps
        // up, and a render thread with nothing to wait for outruns the decode
        // thread by a factor of thousands.
        rules_.pace_us = 500;
        device_ = std::make_unique<FakeSink>(rules_);
    }

    void add(const std::string& name, std::vector<std::uint8_t> bytes,
             const Format& format = cd_audio())
    {
        files_[name] = {format, std::move(bytes)};
    }

    std::unique_ptr<IMedia> open_media(const std::string& path, std::string& why) override
    {
        const auto found = files_.find(path);
        if (found == files_.end()) {
            why = "no decoder recognised it";
            return nullptr;
        }
        return std::make_unique<TapeMedia>(found->second.first, found->second.second,
                                           with_video_.count(path) != 0);
    }

    /// Says this file has a picture in it, so a `Player` test can watch the
    /// video path being opened, started and stopped.
    void add_video(const std::string& name) { with_video_.insert(name); }

    /// §9.7.1's two doors, made of counters. **`VideoPath` cannot tell**, and
    /// that is the claim: it opens both through here, hands one's device to the
    /// other and runs the loop, without ever learning whether the presenter is
    /// Direct3D or two atomics.
    /// Null: the fake presenter has no swap chain and nothing that will show
    /// what it draws, so nothing that says when. A `VideoPath` that wants to
    /// run in a test is handed a clock by the test.
    [[nodiscard]] std::unique_ptr<IFrameClock> frame_clock(Presenter&, void*) override
    {
        return frames_ ? frames_() : nullptr;
    }

    /// What `frame_clock` should answer, when a test wants the picture to run.
    void pace_with(std::function<std::unique_ptr<IFrameClock>()> make)
    {
        frames_ = std::move(make);
    }

    std::unique_ptr<Presenter> open_presenter(void* window, std::string& module,
                                              std::string& why) override
    {
        if (!presenter_) {
            why = "no presenter module is loaded";
            return nullptr;
        }
        auto presenter = std::make_unique<Presenter>();
        if (presenter->open(fake_presenter_vtbl(), window) != MP_OK) {
            why = "video_test would not open a presenter";
            return nullptr;
        }
        module = "video_test";
        return presenter;
    }

    std::unique_ptr<VideoDecoder> open_video_decoder(MpCodec codec,
                                                     const MpGraphicsDevice* device,
                                                     const std::uint8_t* config,
                                                     std::uint32_t config_bytes,
                                                     std::string& module,
                                                     std::string& why) override
    {
        if (!video_codec_) {
            why = "nothing here decodes that video codec";
            return nullptr;
        }
        auto decoder = std::make_unique<VideoDecoder>();
        if (decoder->open(fake_video_codec_vtbl(), codec, device, config, config_bytes) !=
            MP_OK) {
            why = "none of the 1 decoders for that codec would open this stream";
            return nullptr;
        }
        module = "vcodec_test";
        return decoder;
    }

    /// A machine with no presenter, or none that decodes this. Both are real
    /// answers a head gives, and both are error paths worth walking.
    void no_presenter() noexcept { presenter_ = false; }
    void no_video_codec() noexcept { video_codec_ = false; }

    Sink open_sink(const std::string& want, bool shared, std::string& resolved,
                   std::string& why) override
    {
        (void)shared;
        if (!want.empty() && want != "fake") {
            why = "no endpoint matches `" + want + "`";
            return {};
        }
        if (!present_) {
            why = "the device is not there";
            return {};
        }
        resolved = "fake";
        // A fresh device per run, so a test can tell one run's bytes from
        // another's -- which is exactly what a rebuild is.
        device_ = std::make_unique<FakeSink>(rules_);
        return device_->handle();
    }

    [[nodiscard]] const MpDspVtbl* dsp(const std::string&) override { return nullptr; }
    [[nodiscard]] bool device_ready(const std::string&, bool) override { return present_; }

    void log(const std::string& line) override
    {
        const std::lock_guard lock{mutex_};
        lines_.push_back(line);
    }

    [[nodiscard]] std::vector<std::string> lines() const
    {
        const std::lock_guard lock{mutex_};
        return lines_;
    }
    [[nodiscard]] bool said(const std::string& fragment) const
    {
        const std::lock_guard lock{mutex_};
        return std::any_of(lines_.begin(), lines_.end(), [&](const std::string& line) {
            return line.find(fragment) != std::string::npos;
        });
    }

    [[nodiscard]] FakeSink& device() { return *device_; }
    void unplug(bool gone) { present_ = !gone; }

    FakeSinkRules rules_;

private:
    std::map<std::string, std::pair<Format, std::vector<std::uint8_t>>> files_;
    std::set<std::string> with_video_;
    std::function<std::unique_ptr<IFrameClock>()> frames_;
    std::unique_ptr<FakeSink> device_;
    bool present_ = true;
    bool presenter_ = true;
    bool video_codec_ = true;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

/// Polls until something is true, because a fixed sleep in a test about threads
/// is a test that passes on the machine it was written on.
template <typename Predicate>
bool wait_for(Predicate ready, int milliseconds = 4000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

inline bool wait_for_state(const Player& player, ipc::State state, int ms = 4000)
{
    return wait_for([&] { return player.status().state == state; }, ms);
}

} // namespace mp::test

#endif // MEDIAPERCH_TESTS_FAKE_HOST_HPP

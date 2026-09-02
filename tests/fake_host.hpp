// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an operating system looks like to `mp::Player`, in about eighty lines.
//
// That is the useful thing this file says: `IEngineHost` is the whole of what
// the engine asks a platform for, so a platform a test invents is enough to run
// the entire product with no COM, no LoadLibrary and no audio hardware. If this
// file ever grows, the split between the core and the head has moved and
// somebody should ask why.

#ifndef MEDIAPERCH_TESTS_FAKE_HOST_HPP
#define MEDIAPERCH_TESTS_FAKE_HOST_HPP

#include "fake_sink.hpp"

#include "mediaperch/player.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
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

    std::unique_ptr<ISource> open_source(const std::string& path, std::string& decoder,
                                         std::string& why) override
    {
        const auto found = files_.find(path);
        if (found == files_.end()) {
            why = "no decoder recognised it";
            return nullptr;
        }
        decoder = "decode_test";
        return std::make_unique<Tape>(found->second.first, found->second.second);
    }

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
    std::unique_ptr<FakeSink> device_;
    bool present_ = true;
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

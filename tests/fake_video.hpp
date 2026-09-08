// SPDX-License-Identifier: GPL-3.0-or-later
//
// A presenter and a video decoder made of counters, so the assembly around
// them can be tested without a GPU.
//
// **The point is that `mp::VideoPath` never learns which.** It opens both
// through `IEngineHost`, hands one's device to the other, builds the graph and
// runs the loop; whether the presenter is Direct3D or two atomics is a
// question it does not ask. That is the claim this file exists to check, and
// the reason it can be checked on a CI runner with no display.
//
// The real ones are measured elsewhere and on pixels: video_d3d11_test.cpp and
// hdr_transfer_test.cpp render and read back. Nothing here is a substitute for
// that -- these fakes say what was *asked of* a presenter, never what came out
// of one.

#ifndef MEDIAPERCH_TESTS_FAKE_VIDEO_HPP
#define MEDIAPERCH_TESTS_FAKE_VIDEO_HPP

#include "mediaperch/display.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mp::test {

/// What one fake presenter was told and shown.
///
/// Static because the ABI hands out an opaque `MpVideo*` and a test wants to
/// look at the thing afterwards; one at a time is all any test here needs, and
/// `reset` between them is what keeps that honest.
struct PresenterLog {
    std::mutex mutex;
    bool open = false;
    bool configured = false;
    MpVideoInfo info{};
    std::vector<std::pair<std::string, std::string>> settings;
    std::atomic<std::uint64_t> presented{0};
    /// What `set("size", ...)` was last given, or empty if it never was.
    std::string size;
    /// How many stages this presenter was handed (§9.8.3).
    std::uint32_t stages = 0;
    /// How many presenters have been opened since the last `reset`. A track
    /// boundary builds the picture again, and this is how a test sees one.
    std::uint32_t opens = 0;
    /// The composition surface to report, or zero for a presenter that has
    /// none -- which is what a window or an off-screen target answers.
    ///
    /// **Set by the test rather than made here**, because a handle is the
    /// platform's and this header is not: `ipc_server` duplicates whatever this
    /// says into the process that asks, so a test that wants that path taken
    /// puts a real handle of its own here.
    std::uint64_t surface = 0;
    /// Made to refuse, so a caller's error path is a path something takes.
    bool refuse_size = false;
    bool refuse_configure = false;

    void reset()
    {
        const std::lock_guard lock{mutex};
        open = false;
        configured = false;
        info = MpVideoInfo{};
        settings.clear();
        presented.store(0);
        size.clear();
        stages = 0;
        opens = 0;
        surface = 0;
        refuse_size = false;
        refuse_configure = false;
    }

    [[nodiscard]] std::string setting(const std::string& key)
    {
        const std::lock_guard lock{mutex};
        for (auto it = settings.rbegin(); it != settings.rend(); ++it) {
            if (it->first == key) {
                return it->second;
            }
        }
        return {};
    }
};

inline PresenterLog& presenter_log()
{
    static PresenterLog log;
    return log;
}

/// A display that never runs out, so the picture ends when the track does
/// rather than when the clock does.
class EndlessClock final : public mp::IFrameClock {
public:
    bool wait() override
    {
        if (cancelled_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        now_ += 166'667;
        return !cancelled_.load(std::memory_order_acquire);
    }
    [[nodiscard]] double nominal_interval() const override { return 1.0 / 60.0; }
    [[nodiscard]] std::uint64_t now() const override { return now_; }
    [[nodiscard]] std::uint64_t rate() const override { return 10'000'000; }
    void cancel() noexcept override { cancelled_.store(true, std::memory_order_release); }

private:
    std::uint64_t now_ = 0;
    std::atomic<bool> cancelled_{false};
};

namespace detail {

inline MpResult MP_CALL video_open(void* window, MpVideo** out) noexcept
{
    PresenterLog& log = presenter_log();
    {
        const std::lock_guard lock{log.mutex};
        log.open = true;
        ++log.opens;
        // **A presenter that has just been opened has been told nothing.** The
        // log outlives any one of them, so without this a setting from the
        // previous track would answer for the current one -- and what several
        // tests here check is precisely that something is said again.
        log.settings.clear();
        log.size.clear();
        log.stages = 0;
    }
    // The handle is never dereferenced; it only has to be distinguishable
    // from null, because that is all the ABI promises about it.
    *out = reinterpret_cast<MpVideo*>(window != nullptr ? window : &log);
    return MP_OK;
}

inline void MP_CALL video_close(MpVideo*) noexcept
{
    const std::lock_guard lock{presenter_log().mutex};
    presenter_log().open = false;
}

inline MpResult MP_CALL video_configure(MpVideo*, const MpVideoInfo* in) noexcept
{
    PresenterLog& log = presenter_log();
    const std::lock_guard lock{log.mutex};
    if (log.refuse_configure) {
        return MP_ERR_UNSUPPORTED;
    }
    log.configured = true;
    std::memcpy(&log.info, in, std::min<std::size_t>(in->size, sizeof(log.info)));
    return MP_OK;
}

inline MpResult MP_CALL video_present(MpVideo*, const MpVideoFrame*) noexcept
{
    presenter_log().presented.fetch_add(1, std::memory_order_relaxed);
    return MP_OK;
}

inline MpResult MP_CALL video_set(MpVideo*, const char* key, const char* value) noexcept
{
    PresenterLog& log = presenter_log();
    const std::lock_guard lock{log.mutex};
    if (std::strcmp(key, "size") == 0) {
        if (log.refuse_size) {
            return MP_ERR_INVALID;
        }
        log.size = value;
    }
    log.settings.emplace_back(key, value);
    return MP_OK;
}

inline MpResult MP_CALL video_describe(MpVideo*, std::uint32_t index, char* out,
                                       std::uint32_t out_bytes) noexcept
{
    PresenterLog& log = presenter_log();
    const std::lock_guard lock{log.mutex};
    if (index == 0) {
        std::snprintf(out, out_bytes, "size\t%s\twhat it renders at",
                      log.size.empty() ? "native" : log.size.c_str());
        return MP_OK;
    }
    // **Spelled the way the real one spells it**, because what reads this is
    // `VideoPath::surface`, which looks for `composition 0x` and would happily
    // pass a row that meant something else.
    if (index == 1 && log.surface != 0) {
        std::snprintf(out, out_bytes, "surface\tcomposition 0x%llx\twhere it draws",
                      static_cast<unsigned long long>(log.surface));
        return MP_OK;
    }
    return MP_END;
}

inline MpResult MP_CALL video_get_device(MpVideo*, MpGraphicsDevice* out) noexcept
{
    // **No device, which is a real answer.** A presenter with nothing to share
    // says so and the decoder opens in system memory; a fake that invented one
    // would have `VideoPath` hand a decoder a pointer to nothing.
    (void)out;
    return MP_ERR_UNSUPPORTED;
}

inline MpResult MP_CALL video_read_back(MpVideo*, void*, std::size_t, std::uint32_t*,
                                        std::uint32_t*, MpPixelLayout*) noexcept
{
    return MP_ERR_UNSUPPORTED;
}

inline MpResult MP_CALL video_stages(MpVideo*, const MpVideoStage* stages,
                                     std::uint32_t count) noexcept
{
    PresenterLog& log = presenter_log();
    const std::lock_guard lock{log.mutex};
    log.stages = count;
    // Every stage is configured, which is what a real presenter does and what
    // makes "the chain was handed over" checkable without a device.
    for (std::uint32_t i = 0; i < count; ++i) {
        if (stages[i].vtbl == nullptr || stages[i].vtbl->configure == nullptr) {
            return MP_ERR_INVALID;
        }
        MpVideoInfo linear{};
        std::memcpy(&linear, &log.info, sizeof(linear));
        linear.size = sizeof(linear);
        linear.transfer = 8; // linear, which is what §9.8.3 says the chain sees
        MpVideoInfo answered{};
        answered.size = sizeof(answered);
        if (stages[i].vtbl->configure(stages[i].handle, &linear, &answered) != MP_OK) {
            return MP_ERR_UNSUPPORTED;
        }
    }
    return MP_OK;
}

} // namespace detail

inline const MpVideoVtbl& fake_presenter_vtbl()
{
    static const MpVideoVtbl vtbl{sizeof(MpVideoVtbl),
                                  0,
                                  &detail::video_open,
                                  &detail::video_close,
                                  &detail::video_configure,
                                  &detail::video_present,
                                  &detail::video_set,
                                  &detail::video_describe,
                                  &detail::video_get_device,
                                  &detail::video_read_back,
                                  &detail::video_stages};
    return vtbl;
}

/// What one fake decoder was told, and what it hands back.
struct DecoderLog {
    std::mutex mutex;
    bool open = false;
    /// Frames it will part with before saying the stream is over. Zero is a
    /// decoder that never produces one, which is what a loop with nothing to
    /// show looks like.
    std::uint64_t frames = 0;
    std::uint64_t produced = 0;
    /// How many decoders have been opened since the last `reset`. A track
    /// boundary opens one, because the codec may have changed; the presenter
    /// is not opened again, which is what `opens` next door is for.
    std::uint32_t opens = 0;
    /// The timescale the pts below are counted in, matching the info a test
    /// configures the graph with.
    std::uint32_t timescale = 1000;
    /// How far apart the frames are, in those units.
    std::uint64_t step = 40;
    std::string threads;
    /// Made to refuse, so `VideoPath`'s error path is a path something takes.
    bool refuse_threads = false;
    bool refuse_open = false;
    /// What `get_format` answers, when a test gives it something to say: a
    /// `size` of zero is a decoder with no answer, which is the default.
    MpVideoInfo says{};

    void reset()
    {
        const std::lock_guard lock{mutex};
        open = false;
        frames = 0;
        produced = 0;
        opens = 0;
        timescale = 1000;
        step = 40;
        threads.clear();
        refuse_threads = false;
        refuse_open = false;
        says = MpVideoInfo{};
    }
};

inline DecoderLog& decoder_log()
{
    static DecoderLog log;
    return log;
}

namespace detail {

inline MpResult MP_CALL codec_open(MpCodec, const MpGraphicsDevice*, const std::uint8_t*,
                                   std::uint32_t, MpVideoCodec** out) noexcept
{
    DecoderLog& log = decoder_log();
    const std::lock_guard lock{log.mutex};
    if (log.refuse_open) {
        return MP_ERR_UNSUPPORTED;
    }
    log.open = true;
    ++log.opens;
    log.produced = 0;
    *out = reinterpret_cast<MpVideoCodec*>(&log);
    return MP_OK;
}

inline void MP_CALL codec_close(MpVideoCodec*) noexcept
{
    const std::lock_guard lock{decoder_log().mutex};
    decoder_log().open = false;
}

inline MpResult MP_CALL codec_get_format(MpVideoCodec*, MpVideoInfo* out) noexcept
{
    // The container's answer, unchanged, unless a test gave this decoder
    // something to say -- then `VideoGraph` reconciles the two, which is
    // video_path_test.cpp's subject.
    DecoderLog& log = decoder_log();
    const std::lock_guard lock{log.mutex};
    if (out == nullptr || log.says.size == 0) {
        return MP_ERR_UNSUPPORTED;
    }
    std::memcpy(out, &log.says, sizeof(log.says));
    out->size = sizeof(MpVideoInfo);
    return MP_OK;
}

inline MpResult MP_CALL codec_decode(MpVideoCodec*, const void*, std::size_t,
                                     std::uint64_t) noexcept
{
    return MP_OK;
}

inline MpResult MP_CALL codec_next_frame(MpVideoCodec*, MpVideoFrame* out) noexcept
{
    DecoderLog& log = decoder_log();
    const std::lock_guard lock{log.mutex};
    if (log.produced >= log.frames) {
        return MP_END;
    }
    out->width = 16;
    out->height = 16;
    out->layout = MP_LAYOUT_NV12;
    out->pts = log.produced * log.step;
    ++log.produced;
    return MP_OK;
}

inline MpResult MP_CALL codec_flush(MpVideoCodec*) noexcept { return MP_OK; }

inline MpResult MP_CALL codec_reset(MpVideoCodec*) noexcept
{
    const std::lock_guard lock{decoder_log().mutex};
    decoder_log().produced = 0;
    return MP_OK;
}

inline MpResult MP_CALL codec_set(MpVideoCodec*, const char* key,
                                  const char* value) noexcept
{
    DecoderLog& log = decoder_log();
    const std::lock_guard lock{log.mutex};
    if (std::strcmp(key, "threads") != 0) {
        return MP_ERR_UNSUPPORTED;
    }
    if (log.refuse_threads) {
        return MP_ERR_BUSY;
    }
    log.threads = value;
    return MP_OK;
}

} // namespace detail

/// What one fake video stage was told. Static for the reason the presenter's
/// log is: the ABI hands out an opaque handle, and one at a time is all any
/// test here needs.
struct StageLog {
    std::mutex mutex;
    std::uint32_t opened = 0;
    std::uint32_t closed = 0;
    bool configured = false;
    std::uint64_t processed = 0;
    std::string amount{"1"};
    /// Made to refuse, so a caller's error path is a path something takes.
    bool refuse_open = false;

    void reset()
    {
        const std::lock_guard lock{mutex};
        opened = 0;
        closed = 0;
        configured = false;
        processed = 0;
        amount = "1";
        refuse_open = false;
    }
};

inline StageLog& stage_log()
{
    static StageLog log;
    return log;
}

namespace detail {

inline MpResult MP_CALL vdsp_probe(MpGraphicsApi, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // Any API, because this one is made of counters and touches no device.
    *out_score = 50u;
    return MP_OK;
}

inline MpResult MP_CALL vdsp_open(const MpGraphicsDevice*, MpVideoDsp** out) noexcept
{
    StageLog& log = stage_log();
    const std::lock_guard lock{log.mutex};
    if (log.refuse_open) {
        return MP_ERR_UNSUPPORTED;
    }
    ++log.opened;
    *out = reinterpret_cast<MpVideoDsp*>(&log);
    return MP_OK;
}

inline void MP_CALL vdsp_close(MpVideoDsp*) noexcept
{
    const std::lock_guard lock{stage_log().mutex};
    ++stage_log().closed;
}

inline MpResult MP_CALL vdsp_configure(MpVideoDsp*, const MpVideoInfo* in,
                                       MpVideoInfo* out) noexcept
{
    StageLog& log = stage_log();
    const std::lock_guard lock{log.mutex};
    log.configured = true;
    std::memcpy(out, in, std::min<std::size_t>(in->size, out->size));
    return MP_OK;
}

inline MpResult MP_CALL vdsp_process(MpVideoDsp*, const MpVideoFrame* in,
                                     MpVideoFrame* out) noexcept
{
    stage_log().processed += 1;
    // Handed back unchanged, which is what a stage with nothing to do does and
    // what an identity lookup table does too.
    std::memcpy(out, in, std::min<std::size_t>(in->size, out->size));
    return MP_OK;
}

inline MpResult MP_CALL vdsp_reset(MpVideoDsp*) noexcept { return MP_OK; }

inline MpResult MP_CALL vdsp_set(MpVideoDsp*, const char* key, const char* value) noexcept
{
    if (std::strcmp(key, "amount") != 0) {
        return MP_ERR_UNSUPPORTED;
    }
    const std::lock_guard lock{stage_log().mutex};
    stage_log().amount = value;
    return MP_OK;
}

inline MpResult MP_CALL vdsp_describe(MpVideoDsp*, std::uint32_t index, char* out,
                                      std::uint32_t out_bytes) noexcept
{
    if (index != 0) {
        return MP_END;
    }
    const std::lock_guard lock{stage_log().mutex};
    std::snprintf(out, out_bytes, "amount\t%s\twhat this fake stage was told",
                  stage_log().amount.c_str());
    return MP_OK;
}

} // namespace detail

inline const MpVideoDspVtbl& fake_video_stage_vtbl()
{
    static const MpVideoDspVtbl vtbl{sizeof(MpVideoDspVtbl),
                                     0,
                                     &detail::vdsp_probe,
                                     &detail::vdsp_open,
                                     &detail::vdsp_close,
                                     &detail::vdsp_configure,
                                     &detail::vdsp_process,
                                     &detail::vdsp_reset,
                                     &detail::vdsp_set,
                                     &detail::vdsp_describe};
    return vtbl;
}

inline const MpVideoCodecVtbl& fake_video_codec_vtbl()
{
    static const MpVideoCodecVtbl vtbl{sizeof(MpVideoCodecVtbl),
                                       0,
                                       nullptr, /* probe */
                                       &detail::codec_open,
                                       &detail::codec_close,
                                       &detail::codec_get_format,
                                       &detail::codec_decode,
                                       &detail::codec_next_frame,
                                       &detail::codec_flush,
                                       &detail::codec_reset,
                                       &detail::codec_set};
    return vtbl;
}

} // namespace mp::test

#endif // MEDIAPERCH_TESTS_FAKE_VIDEO_HPP

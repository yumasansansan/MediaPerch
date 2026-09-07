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

#include <mediaperch/module.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

namespace detail {

inline MpResult MP_CALL video_open(void* window, MpVideo** out) noexcept
{
    PresenterLog& log = presenter_log();
    {
        const std::lock_guard lock{log.mutex};
        log.open = true;
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
    if (index != 0) {
        return MP_END;
    }
    PresenterLog& log = presenter_log();
    const std::lock_guard lock{log.mutex};
    std::snprintf(out, out_bytes, "size\t%s\twhat it renders at",
                  log.size.empty() ? "native" : log.size.c_str());
    return MP_OK;
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
                                  &detail::video_read_back};
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
    /// The timescale the pts below are counted in, matching the info a test
    /// configures the graph with.
    std::uint32_t timescale = 1000;
    /// How far apart the frames are, in those units.
    std::uint64_t step = 40;
    std::string threads;
    /// Made to refuse, so `VideoPath`'s error path is a path something takes.
    bool refuse_threads = false;
    bool refuse_open = false;

    void reset()
    {
        const std::lock_guard lock{mutex};
        open = false;
        frames = 0;
        produced = 0;
        timescale = 1000;
        step = 40;
        threads.clear();
        refuse_threads = false;
        refuse_open = false;
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
    // The container's answer, unchanged: a decoder that disagreed would send
    // `VideoGraph` down its reconfigure path, which is another test's subject.
    (void)out;
    return MP_ERR_UNSUPPORTED;
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

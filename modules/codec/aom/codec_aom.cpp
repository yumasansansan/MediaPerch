// SPDX-License-Identifier: GPL-3.0-or-later
//
// AV1 through libaom, which is here to be checked against rather than to be
// played.
//
// **The reference implementation, and that is the whole argument.** dav1d is
// faster and is what a player should use; libaom is what AV1 *means*. The
// decoding process is defined bit-exactly, so two conformant decoders must agree
// on every sample of every frame -- and two independent implementations agreeing
// byte for byte rules out almost everything a single decoder's own tests can
// only gesture at. `codec_dav1d_test.cpp` on its own can say a frame has a
// spread of luminance and more than one hue; that rules out a cleared buffer and
// very little else.
//
// This is the method §12 already uses for audio arriving for video: one decoder
// against another, and both against what was encoded.
//
// **It scores low on purpose.** A reference decoder is slow by construction --
// clarity over speed is what makes it a reference -- so nothing should pick it
// to play a file. §7's first rule is that an explicit choice wins outright,
// which is how a person asks for it when they want the reference answer.

#include <mediaperch/module.h>

#include "decoder_threads.hpp"

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace {

const MpHost* g_host = nullptr;

void log_line(MpLogLevel level, const char* msg) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, msg);
    }
}

constexpr std::size_t k_av1c_fixed = 4;
constexpr std::uint8_t k_av1c_marker = 0x81u;

} // namespace

struct MpVideoCodec {
    ~MpVideoCodec()
    {
        if (started) {
            aom_codec_destroy(&ctx);
        }
    }

    aom_codec_ctx_t ctx{};
    bool started = false;

    /// The image handed out by the last `next_frame`. libaom owns it and keeps
    /// it valid until the next `aom_codec_decode`, which is exactly the promise
    /// the ABI makes to its own caller.
    const aom_image_t* image = nullptr;
    /// Where `aom_codec_get_frame` is up to within one decode call.
    aom_codec_iter_t iter = nullptr;

    /// The configuration OBUs out of `av1C`, kept because a reset sends them
    /// again.
    std::string config;

    MpVideoInfo info{};
    bool have_format = false;
    std::string trouble;
};

namespace {

/// The layout of a decoded image, in the ABI's words.
///
/// libaom puts high bit depth samples in the **low** bits of each sixteen, the
/// same way dav1d does and the opposite way from P010 -- so `shift` is zero and
/// `mp_pixel_sample_scale` does the rest. See MpPixelLayout.
MpPixelLayout layout_of(const aom_image_t& img) noexcept
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    if (img.monochrome != 0) {
        out.chroma = MP_CHROMA_MONO;
    } else {
        switch (img.fmt & static_cast<unsigned>(~AOM_IMG_FMT_HIGHBITDEPTH)) {
        case AOM_IMG_FMT_I422:
            out.chroma = MP_CHROMA_422;
            break;
        case AOM_IMG_FMT_I444:
            out.chroma = MP_CHROMA_444;
            break;
        case AOM_IMG_FMT_I420:
        default:
            out.chroma = MP_CHROMA_420;
            break;
        }
    }
    out.packing = MP_PACK_PLANAR;
    out.bits = img.bit_depth;
    out.container_bits = (img.fmt & AOM_IMG_FMT_HIGHBITDEPTH) != 0 ? 16u : 8u;
    out.shift = 0;
    return out;
}

/// What the image says about its colour, in the code points MpVideoInfo carries.
///
/// AV1 states ISO/IEC 23091-2 code points and libaom passes them through with
/// the same numbering, so this is a copy. Unspecified is 2, which is what an
/// image whose stream said nothing reports.
void read_image_format(MpVideoCodec* c, const aom_image_t& img) noexcept
{
    const std::uint32_t size = c->info.size != 0 ? c->info.size : sizeof(MpVideoInfo);
    c->info = MpVideoInfo{};
    c->info.size = size;
    c->info.width = img.d_w;
    c->info.height = img.d_h;
    c->info.display_width = img.d_w;
    c->info.display_height = img.d_h;
    c->info.primaries = static_cast<std::uint32_t>(img.cp);
    c->info.transfer = static_cast<std::uint32_t>(img.tc);
    c->info.matrix = static_cast<std::uint32_t>(img.mc);
    c->info.flags = img.range == AOM_CR_FULL_RANGE ? MP_VIDEO_FULL_RANGE : 0u;
    // Zero means unchanged rather than untimed: libaom does not re-time, so the
    // demuxer's timescale still holds. See codec_dav1d, which says the same.
    c->info.timescale = 0;
    c->have_format = true;
}

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

MpResult MP_CALL codec_probe(MpCodec codec, MpGraphicsApi api, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_AV1) {
        return MP_OK;
    }
    if (config != nullptr && config_bytes != 0 &&
        (config_bytes < k_av1c_fixed || config[0] != k_av1c_marker)) {
        return MP_OK;
    }
    (void)api;
    // **40, well under dav1d's 100.** This decodes correctly and slowly, which
    // is what a reference is for; nothing should choose it to play a file, and
    // §7's rule that an explicit choice wins outright is how a person asks for
    // it when they want the reference answer. The number is the same whatever
    // the graphics API, because libaom is CPU either way.
    *out_score = 40u;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    if (out == nullptr || codec != MP_CODEC_AV1) {
        return MP_ERR_INVALID;
    }
    (void)device; // CPU, like every software decoder: see plan.md §9.8.2.

    auto c = std::unique_ptr<MpVideoCodec>(new (std::nothrow) MpVideoCodec());
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->info.size = sizeof(MpVideoInfo);

    aom_codec_dec_cfg_t cfg{};
    // Not zero: libaom reads that as one thread. See modules/shared/decoder_threads.
    cfg.threads = mp::decoder_threads();
    cfg.allow_lowbitdepth = 1;
    if (aom_codec_dec_init(&c->ctx, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
        log_line(MP_LOG_ERROR, "codec_aom: libaom would not start");
        return MP_ERR_UNSUPPORTED;
    }
    c->started = true;

    // The configuration OBUs, which are the sequence header. Four fixed bytes
    // and then OBUs, sent exactly as they would be mid-stream -- AV1 samples in
    // an MP4 already carry their own sizes, so nothing is rewritten.
    if (config != nullptr && config_bytes > k_av1c_fixed) {
        if (config[0] != k_av1c_marker) {
            log_line(MP_LOG_DEBUG, "codec_aom: the av1C record has no marker byte");
            return MP_ERR_FORMAT;
        }
        c->config.assign(reinterpret_cast<const char*>(config) + k_av1c_fixed,
                         config_bytes - k_av1c_fixed);
        aom_codec_decode(&c->ctx,
                         reinterpret_cast<const std::uint8_t*>(c->config.data()),
                         c->config.size(), nullptr);
    }

    *out = c.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL codec_close(MpVideoCodec* c) noexcept
{
    delete c;
}

MpResult MP_CALL codec_get_format(MpVideoCodec* c, MpVideoInfo* out) noexcept
{
    if (c == nullptr || out == nullptr || out->size < sizeof(std::uint32_t)) {
        return MP_ERR_INVALID;
    }
    if (!c->have_format) {
        // **Unlike dav1d, this cannot answer before the first frame.** libaom
        // has no public way to parse a sequence header without decoding, so the
        // geometry arrives with the picture. A host that needs it earlier asks
        // the demuxer, which read it out of the container.
        return MP_ERR_BUSY;
    }
    const std::uint32_t room = out->size;
    const std::uint32_t copy = room < sizeof(MpVideoInfo) ? room : sizeof(MpVideoInfo);
    std::memcpy(out, &c->info, copy);
    out->size = room;
    return MP_OK;
}

MpResult MP_CALL codec_decode(MpVideoCodec* c, const void* packet, std::size_t bytes,
                              std::uint64_t pts) noexcept
try {
    if (c == nullptr || packet == nullptr || bytes == 0) {
        return MP_ERR_INVALID;
    }
    // **libaom takes a whole packet or fails**, so there is no partial state to
    // carry and no MP_ERR_BUSY to report. The timestamp goes through
    // `user_priv`, which libaom hands back on the image it produced.
    c->image = nullptr;
    c->iter = nullptr;
    const aom_codec_err_t r = aom_codec_decode(
        &c->ctx, static_cast<const std::uint8_t*>(packet), bytes,
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(pts)));
    if (r != AOM_CODEC_OK) {
        const char* detail = aom_codec_error_detail(&c->ctx);
        c->trouble = detail != nullptr ? detail : "libaom could not decode this packet";
        return MP_ERR_FORMAT;
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_next_frame(MpVideoCodec* c, MpVideoFrame* out) noexcept
try {
    if (c == nullptr || out == nullptr || out->size < sizeof(MpVideoFrame)) {
        return MP_ERR_INVALID;
    }
    const aom_image_t* img = aom_codec_get_frame(&c->ctx, &c->iter);
    if (img == nullptr) {
        return MP_END;
    }
    c->image = img;
    read_image_format(c, *img);

    const std::uint32_t size = out->size;
    *out = MpVideoFrame{};
    out->size = size;
    out->width = img->d_w;
    out->height = img->d_h;
    out->layout = layout_of(*img);
    out->pts = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(img->user_priv));

    const std::uint32_t planes = mp_pixel_planes(&out->layout);
    for (std::uint32_t i = 0; i < planes; ++i) {
        out->plane[i] = img->planes[i];
        out->stride[i] = static_cast<std::uint32_t>(img->stride[i]);
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_flush(MpVideoCodec* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // A null packet is how libaom is told the stream ended; what it is holding
    // then comes out of `aom_codec_get_frame` the way everything else does.
    c->iter = nullptr;
    aom_codec_decode(&c->ctx, nullptr, 0, nullptr);
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    c->image = nullptr;
    c->iter = nullptr;
    // libaom has no flush that keeps the context usable for a new position, so
    // a seek starts a decoder again -- which is cheap next to the decode.
    if (c->started) {
        aom_codec_destroy(&c->ctx);
        c->started = false;
    }
    aom_codec_dec_cfg_t cfg{};
    cfg.threads = mp::decoder_threads();
    cfg.allow_lowbitdepth = 1;
    if (aom_codec_dec_init(&c->ctx, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
        return MP_ERR_INTERNAL;
    }
    c->started = true;
    if (!c->config.empty()) {
        aom_codec_decode(&c->ctx,
                         reinterpret_cast<const std::uint8_t*>(c->config.data()),
                         c->config.size(), nullptr);
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

constexpr MpVideoCodecVtbl k_vtbl = {
    /* size       */ sizeof(MpVideoCodecVtbl),
    /* reserved   */ 0,
    /* probe      */ &codec_probe,
    /* open       */ &codec_open,
    /* close      */ &codec_close,
    /* get_format */ &codec_get_format,
    /* decode     */ &codec_decode,
    /* next_frame */ &codec_next_frame,
    /* flush      */ &codec_flush,
    /* reset      */ &codec_reset,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpCodec k_codecs[] = {MP_CODEC_AV1};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VCODEC,
    // Below dav1d's 100, for the reason `probe` gives: correct and slow is what
    // a reference is, and nothing should pick it to play a file.
    /* priority    */ 40,
    /* id          */ "codec_aom",
    /* name        */ "AV1, by the reference decoder",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &k_vtbl,
    /* codecs      */ k_codecs,
    /* codec_count */ 1,
    /* reserved    */ 0,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

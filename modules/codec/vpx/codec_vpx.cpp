// SPDX-License-Identifier: GPL-3.0-or-later
//
// VP8 and VP9, by the reference implementation.
//
// **libvpx is what these codecs mean**, which is §7's argument for libFLAC and
// the Xiph decoders arriving for video: where a reference implementation is the
// specification, it is worth a dependency. Both codecs, because reading only
// the newer one would leave half the reason it is here -- and because VP8 in a
// WebM is a file people still have.
//
// One decoder object per codec, chosen at `open` from the `MpCodec` the
// container named: `vpx_codec_vp8_dx()` and `vpx_codec_vp9_dx()` are different
// interfaces behind one API, and asking the wrong one produces a stream of
// errors rather than a refusal.
//
// **High bit depth is a build option and this module depends on it.**
// `--enable-vp9-highbitdepth` is off in libvpx by default, and without it VP9
// profiles 2 and 3 -- ten and twelve bits, 4:2:2 and 4:4:4 -- do not decode at
// all. ABI v4 can describe those; see this module's CMakeLists for where the
// option is set.

#include <mediaperch/module.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

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

/// The libvpx interface for a codec this module claims, or NULL.
vpx_codec_iface_t* interface_of(MpCodec codec) noexcept
{
    switch (codec) {
    case MP_CODEC_VP8:
        return vpx_codec_vp8_dx();
    case MP_CODEC_VP9:
        return vpx_codec_vp9_dx();
    default:
        return nullptr;
    }
}

} // namespace

struct MpVideoCodec {
    ~MpVideoCodec()
    {
        if (started) {
            vpx_codec_destroy(&ctx);
        }
    }

    vpx_codec_ctx_t ctx{};
    bool started = false;
    MpCodec codec = MP_CODEC_UNKNOWN;

    /// The image handed out by the last `next_frame`. libvpx owns it and keeps
    /// it valid until the next decode, which is the promise the ABI makes on.
    const vpx_image_t* image = nullptr;
    /// Where `vpx_codec_get_frame` is up to within one decode call.
    vpx_codec_iter_t iter = nullptr;
    /// **The timestamp of the packet being decoded.** libvpx takes a
    /// `deadline` and a `user_priv` but does not hand `user_priv` back on the
    /// image, so unlike dav1d and libaom the timestamp is carried here. VP8 and
    /// VP9 in the containers this tree reads produce one frame per packet, so
    /// one is enough -- and a stream that did otherwise would show as a
    /// timestamp repeated rather than as silence.
    std::uint64_t pts = 0;

    MpVideoInfo info{};
    bool have_format = false;
    std::string trouble;
};

namespace {

/// The layout of a decoded image, in the ABI's words.
///
/// libvpx puts high bit depth samples in the low bits of each sixteen, the same
/// way dav1d and libaom do and the opposite way from P010 -- so `shift` is zero
/// and `mp_pixel_sample_scale` does the rest.
MpPixelLayout layout_of(const vpx_image_t& img) noexcept
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    switch (img.fmt) {
    case VPX_IMG_FMT_I422:
    case VPX_IMG_FMT_I42216:
        out.chroma = MP_CHROMA_422;
        break;
    case VPX_IMG_FMT_I444:
    case VPX_IMG_FMT_I44416:
        out.chroma = MP_CHROMA_444;
        break;
    case VPX_IMG_FMT_I440:
    case VPX_IMG_FMT_I44016:
        // 4:4:0 -- full width, half height. VP9 can code it and nothing else in
        // this tree can describe it, so it is named as unreachable rather than
        // reported as something it is not.
        out.chroma = MP_CHROMA_444;
        out.bits = 0; // makes `presentable` refuse it, with the frame saying why
        return out;
    default:
        out.chroma = MP_CHROMA_420;
        break;
    }
    out.packing = MP_PACK_PLANAR;
    out.bits = img.bit_depth;
    out.container_bits = (img.fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0 ? 16u : 8u;
    out.shift = 0;
    return out;
}

/// What the image says about its colour, in the code points MpVideoInfo carries.
///
/// **VP9 states ISO/IEC 23091-2 code points; VP8 does not state anything.** A
/// VP8 stream has no colour description at all -- the format predates the
/// question -- so libvpx reports `VPX_CS_UNKNOWN` and this reports unspecified,
/// which is 2. The container is then the only source, which is §9.8's join
/// working the way it is meant to.
void read_image_format(MpVideoCodec* c, const vpx_image_t& img) noexcept
{
    const std::uint32_t size = c->info.size != 0 ? c->info.size : sizeof(MpVideoInfo);
    c->info = MpVideoInfo{};
    c->info.size = size;
    c->info.width = img.d_w;
    c->info.height = img.d_h;
    c->info.display_width = img.d_w;
    c->info.display_height = img.d_h;

    // libvpx's colour space values are not the code points: it has its own
    // short enumeration, and mapping it is the whole of the translation.
    switch (img.cs) {
    case VPX_CS_BT_601:
        c->info.primaries = 6;
        c->info.transfer = 6;
        c->info.matrix = 6;
        break;
    case VPX_CS_BT_709:
        c->info.primaries = 1;
        c->info.transfer = 1;
        c->info.matrix = 1;
        break;
    case VPX_CS_SMPTE_170:
        c->info.primaries = 6;
        c->info.transfer = 6;
        c->info.matrix = 6;
        break;
    case VPX_CS_SMPTE_240:
        c->info.primaries = 7;
        c->info.transfer = 7;
        c->info.matrix = 7;
        break;
    case VPX_CS_BT_2020:
        c->info.primaries = 9;
        c->info.transfer = 14;
        c->info.matrix = 9;
        break;
    case VPX_CS_SRGB:
        c->info.primaries = 1;
        c->info.transfer = 13;
        c->info.matrix = 0;
        break;
    default:
        c->info.primaries = 2;
        c->info.transfer = 2;
        c->info.matrix = 2;
        break;
    }
    c->info.flags = img.range == VPX_CR_FULL_RANGE ? MP_VIDEO_FULL_RANGE : 0u;
    // Unchanged rather than untimed: this decoder does not re-time, so the
    // demuxer's timescale still holds. See codec_dav1d and codec_aom.
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
    if (interface_of(codec) == nullptr) {
        return MP_OK;
    }
    // VP8 and VP9 in Matroska carry no codec private data, so there is nothing
    // to inspect and nothing to decline on.
    (void)config;
    (void)config_bytes;
    (void)api;
    // **100, unlike libaom's 40**, and the difference is that this has no
    // faster sibling. libaom scores low because dav1d exists and is what a
    // player should use; nothing else here reads VP8 or VP9 at all, so the
    // reference is also the answer.
    *out_score = 100u;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    vpx_codec_iface_t* iface = interface_of(codec);
    if (out == nullptr || iface == nullptr) {
        return MP_ERR_INVALID;
    }
    (void)device; // CPU, like every software decoder: see plan.md §9.8.2.
    (void)config;
    (void)config_bytes;

    auto c = std::unique_ptr<MpVideoCodec>(new (std::nothrow) MpVideoCodec());
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->codec = codec;
    c->info.size = sizeof(MpVideoInfo);

    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 0; // libvpx picks
    if (vpx_codec_dec_init(&c->ctx, iface, &cfg, 0) != VPX_CODEC_OK) {
        log_line(MP_LOG_ERROR, "codec_vpx: libvpx would not start");
        return MP_ERR_UNSUPPORTED;
    }
    c->started = true;

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
        // Like libaom and unlike dav1d: there is no configuration record to
        // read, so the geometry arrives with the first picture. A host that
        // needs it sooner asks the demuxer.
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
    c->image = nullptr;
    c->iter = nullptr;
    c->pts = pts;
    const vpx_codec_err_t r = vpx_codec_decode(
        &c->ctx, static_cast<const std::uint8_t*>(packet),
        static_cast<unsigned int>(bytes), nullptr, 0);
    if (r != VPX_CODEC_OK) {
        const char* detail = vpx_codec_error_detail(&c->ctx);
        c->trouble = detail != nullptr ? detail : vpx_codec_err_to_string(r);
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
    const vpx_image_t* img = vpx_codec_get_frame(&c->ctx, &c->iter);
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
    out->pts = c->pts;

    if (out->layout.bits == 0) {
        c->trouble = "libvpx produced a 4:4:0 image, which nothing here can describe";
        return MP_ERR_UNSUPPORTED;
    }

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
    // Nothing to tell libvpx: it produces a frame per decode call and holds
    // none back, so `next_frame` returning MP_END already means what flush
    // means. VP9's show-existing-frame is resolved inside the decoder.
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    c->image = nullptr;
    c->iter = nullptr;
    c->pts = 0;
    // libvpx has no flush that leaves a context usable at a new position, so a
    // seek starts a decoder again -- cheap next to the decode, and the same
    // answer codec_dav1d's reset reaches for its own reasons.
    vpx_codec_iface_t* iface = interface_of(c->codec);
    if (iface == nullptr) {
        return MP_ERR_INVALID;
    }
    if (c->started) {
        vpx_codec_destroy(&c->ctx);
        c->started = false;
    }
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 0;
    if (vpx_codec_dec_init(&c->ctx, iface, &cfg, 0) != VPX_CODEC_OK) {
        return MP_ERR_INTERNAL;
    }
    c->started = true;
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

const MpCodec k_codecs[] = {MP_CODEC_VP8, MP_CODEC_VP9};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VCODEC,
    /* priority    */ 100,
    /* id          */ "codec_vpx",
    /* name        */ "VP8 and VP9, by the reference decoder",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &k_vtbl,
    /* codecs      */ k_codecs,
    /* codec_count */ 2,
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

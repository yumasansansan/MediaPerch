// SPDX-License-Identifier: GPL-3.0-or-later
//
// AV2 through avm, which is the only decoder AV2 has.
//
// **AV2 reached 1.0.0 on 2026-05-29 and nothing else reads it yet.** dav2d
// exists and is where a player's AV2 decoder will come from, the way dav1d is
// for AV1 -- it is a submodule here already and has no module yet. Until it
// does, avm is not "the reference to check the fast one against": it is the
// whole of AV2 support, and it is here on that footing.
//
// **It still scores 40.** A reference implementation is correct and slow by
// construction, and the day `codec_dav2d` arrives it should outrank this
// without anything here needing an edit. Scoring it 100 now would mean editing
// two modules later to say what is already true today. A host with one AV2
// decoder picks it at 40 exactly as it would at 100; the number only decides
// between rivals.
//
// **This file is codec_aom with the names changed, and it is not shared with
// it.** avm is a fork of libaom, so the two APIs are the same shape down to the
// field names -- `d_w`, `cp`, `tc`, `mc`, `user_priv` -- and about sixty lines
// here are a rename away from being identical. Sharing them would mean a
// template over two libraries' enumerations, asserting that AVM_IMG_FMT_I422
// and AOM_IMG_FMT_I422 will keep the same value in two projects that version
// independently. avm is a research codebase with a stated intent to diverge;
// the duplication is the cheaper of the two mistakes, and it is a deliberate
// one rather than an oversight.
//
// **Its configuration record is four bytes and holds nothing to feed.** avm's
// own muxer writes an Av2Config into Matroska's CodecPrivate -- the AV2
// analogue of `av1C`'s fixed header -- and `get_av2config_from_obu` explicitly
// does not carry the `configOBUs` an `av1C` does. So the sequence header lives
// only in the stream, which is where this reads it: unlike `codec_aom` there is
// nothing to replay into the decoder at open or after a reset.

#include <mediaperch/module.h>

#include "decoder_threads.hpp"

#include <avm/avm_decoder.h>
#include <avm/avmdx.h>

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

/// The Av2Config record avm writes into Matroska's CodecPrivate: a marker and
/// version byte, then three bytes of profile, level and bit depth. Checked for
/// its shape and then not used -- see the note at the top of this file.
constexpr std::size_t k_av2c_bytes = 4;
constexpr std::uint8_t k_av2c_marker = 0x81u;

} // namespace

struct MpVideoCodec {
    ~MpVideoCodec()
    {
        if (started) {
            avm_codec_destroy(&ctx);
        }
    }

    avm_codec_ctx_t ctx{};
    bool started = false;

    /// The image handed out by the last `next_frame`. avm owns it and keeps it
    /// valid until the next `avm_codec_decode`, which is exactly the promise the
    /// ABI makes to its own caller.
    const avm_image_t* image = nullptr;
    /// Where `avm_codec_get_frame` is up to within one decode call.
    avm_codec_iter_t iter = nullptr;

    MpVideoInfo info{};
    bool have_format = false;
    std::string trouble;
};

namespace {

/// The layout of a decoded image, in the ABI's words.
///
/// avm puts high bit depth samples in the **low** bits of each sixteen, the same
/// way libaom, dav1d and libvpx do and the opposite way from P010 -- so `shift`
/// is zero and `mp_pixel_sample_scale` does the rest. See MpPixelLayout.
MpPixelLayout layout_of(const avm_image_t& img) noexcept
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    if (img.monochrome != 0) {
        out.chroma = MP_CHROMA_MONO;
    } else {
        switch (img.fmt & static_cast<unsigned>(~AVM_IMG_FMT_HIGHBITDEPTH)) {
        case AVM_IMG_FMT_I422:
            out.chroma = MP_CHROMA_422;
            break;
        case AVM_IMG_FMT_I444:
            out.chroma = MP_CHROMA_444;
            break;
        case AVM_IMG_FMT_I420:
        default:
            out.chroma = MP_CHROMA_420;
            break;
        }
    }
    out.packing = MP_PACK_PLANAR;
    out.bits = img.bit_depth;
    out.container_bits = (img.fmt & AVM_IMG_FMT_HIGHBITDEPTH) != 0 ? 16u : 8u;
    out.shift = 0;
    return out;
}

/// What the image says about its colour, in the code points MpVideoInfo carries.
///
/// AV2 states ISO/IEC 23091-2 code points, as AV1 does, and avm passes them
/// through with the same numbering -- so this is a copy. Unspecified is 2, which
/// is what an image whose stream said nothing reports.
void read_image_format(MpVideoCodec* c, const avm_image_t& img) noexcept
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
    c->info.flags = img.range == AVM_CR_FULL_RANGE ? MP_VIDEO_FULL_RANGE : 0u;
    // Zero means unchanged rather than untimed: avm does not re-time, so the
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
    if (codec != MP_CODEC_AV2) {
        return MP_OK;
    }
    // A record that is present must be an Av2Config, and there is exactly one
    // shape it can have: four bytes beginning with the marker. Anything else in
    // that field means the container and this module disagree about what AV2
    // is, which is a thing to decline rather than to guess at.
    if (config != nullptr && config_bytes != 0 &&
        (config_bytes != k_av2c_bytes || config[0] != k_av2c_marker)) {
        return MP_OK;
    }
    (void)api;
    // **40, not 100, though nothing else decodes AV2 today.** See the top of
    // this file: the score is what decides between rivals, and dav2d should win
    // when it arrives without an edit here.
    *out_score = 40u;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    if (out == nullptr || codec != MP_CODEC_AV2) {
        return MP_ERR_INVALID;
    }
    (void)device; // CPU, like every software decoder: see plan.md §9.8.2.

    if (config != nullptr && config_bytes != 0 &&
        (config_bytes != k_av2c_bytes || config[0] != k_av2c_marker)) {
        log_line(MP_LOG_DEBUG, "codec_avm: that is not an Av2Config record");
        return MP_ERR_FORMAT;
    }

    auto c = std::unique_ptr<MpVideoCodec>(new (std::nothrow) MpVideoCodec());
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->info.size = sizeof(MpVideoInfo);

    // **No `allow_lowbitdepth` here, and libaom has one.** avm dropped the
    // field, which says its decoder always takes the high bit depth path -- so
    // an eight-bit stream comes back as eight bits in a sixteen-bit container.
    // That is a shape ABI v4 can state and v3 could not, and it is the reason
    // `container_bits` is read from the image rather than assumed.
    avm_codec_dec_cfg_t cfg{};
    // Not zero: avm reads that as one thread. See modules/shared/decoder_threads.
    cfg.threads = mp::decoder_threads();
    if (avm_codec_dec_init(&c->ctx, avm_codec_av2_dx(), &cfg, 0) != AVM_CODEC_OK) {
        log_line(MP_LOG_ERROR, "codec_avm: avm would not start");
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
        // Like libaom and unlike dav1d, this cannot answer before the first
        // frame: there is no public way to parse a sequence header without
        // decoding one. A host that needs the geometry earlier asks the
        // demuxer, which read it out of the container.
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
    // avm takes a whole packet or fails, so there is no partial state to carry
    // and no MP_ERR_BUSY to report. The timestamp goes through `user_priv`,
    // which avm hands back on the image it produced.
    c->image = nullptr;
    c->iter = nullptr;
    const avm_codec_err_t r = avm_codec_decode(
        &c->ctx, static_cast<const std::uint8_t*>(packet), bytes,
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(pts)));
    if (r != AVM_CODEC_OK) {
        const char* detail = avm_codec_error_detail(&c->ctx);
        c->trouble = detail != nullptr ? detail : "avm could not decode this packet";
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
    const avm_image_t* img = avm_codec_get_frame(&c->ctx, &c->iter);
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
    // A null packet is how avm is told the stream ended; what it is holding then
    // comes out of `avm_codec_get_frame` the way everything else does.
    c->iter = nullptr;
    avm_codec_decode(&c->ctx, nullptr, 0, nullptr);
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    c->image = nullptr;
    c->iter = nullptr;
    // avm has no flush that keeps the context usable for a new position, so a
    // seek starts a decoder again -- which is cheap next to the decode. There is
    // no configuration record to replay afterwards: the sequence header is in
    // the stream, and a seek lands on a keyframe that carries one.
    if (c->started) {
        avm_codec_destroy(&c->ctx);
        c->started = false;
    }
    avm_codec_dec_cfg_t cfg{};
    cfg.threads = mp::decoder_threads();
    if (avm_codec_dec_init(&c->ctx, avm_codec_av2_dx(), &cfg, 0) != AVM_CODEC_OK) {
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

const MpCodec k_codecs[] = {MP_CODEC_AV2};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VCODEC,
    // The same 40 codec_aom carries, and for the same reason: this is the
    // reference, and dav2d is where a player's AV2 decoder will come from.
    /* priority    */ 40,
    /* id          */ "codec_avm",
    /* name        */ "AV2, by the reference decoder",
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

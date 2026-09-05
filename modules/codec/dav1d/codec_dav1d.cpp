// SPDX-License-Identifier: GPL-3.0-or-later
//
// AV1, decoded by dav1d, and the first video codec in this tree that is not the
// operating system's.
//
// **Why this one rather than H.264.** Three reasons, and they compound. AV1 is
// the codec Windows cannot decode without a Store extension, so this module
// adds capability rather than a second way to do what Media Foundation already
// does. dav1d is BSD-2 and AV1 is royalty-free by design, so a GPLv3 tree can
// ship the source without the patent-pool problem every H.264 decoder carries.
// And dav1d produces 4:0:0, 4:2:0, 4:2:2 and 4:4:4 at 8, 10 and 12 bits --
// which is what made ABI v4 describe a frame rather than name it, and is the
// producer that keeps the presenter's planar paths from being written blind.
// See plan.md §9.8.2.
//
// **The bits come back at the bottom of the container, and that is the whole
// point of `MpPixelLayout::shift`.** dav1d states it plainly: ten-bit samples
// are in the LSBs with the upper bits zeroed. P010, from the hardware decoder
// this tree already had, puts the same ten bits at the *top* of the same
// sixteen. Same depth, same container, same chroma, and a factor of sixty-four
// between them -- as brightness, not as an error. v3 chose between the two with
// a `bool ten_bit` and had no way to be right about both.
//
// **MEDIAPERCH_ARCH does not reach this module, and that is the right answer
// rather than a gap.** dav1d builds every SIMD variant it has and picks one
// with CPUID, so the compiler's baseline never touches its assembly.
// `dav1d_set_cpu_flags_mask` could cap it, and doing so was tried and removed,
// because measuring what it actually does answered the question:
//
//   - the dispatch is a **cascade of overwrites**, not a choice of one tier.
//     A DSP init assigns the SSSE3 functions, then overwrites the ones that
//     have AVX2 versions, then the ones that have AVX-512 versions. Every
//     function with no AVX2 version keeps its SSE one and runs it. So an AVX2
//     build goes through SSE code paths no matter what the mask says, and
//     nothing short of dav1d implementing everything twice would change that;
//   - masking AVX2 and AVX-512 *in* is dav1d's default, so on the `avx2` build
//     the call was exactly a no-op;
//   - masking them *out* on the `baseline` build is the only thing it did do,
//     and it made that build slower on a modern CPU for no correctness gain --
//     dav1d's assembly is bit-exact against its C.
//
// Which is the policy cmake/CompilerOptions.cmake already records for libFLAC,
// libmpg123, libopus and libwavpack: a library that dispatches internally is
// left to dispatch. This tree builds *its own* inner loops twice because it
// controls that codegen; dav1d's is not ours to pick.
//
// **No device, and that is not a shortcoming.** Entropy decoding is serial by
// construction -- AV1's symbol decoder, like CABAC -- and dav1d is CPU with
// hand-written assembly deliberately. What belongs on the GPU is everything
// after the decode, which is where the colour conversion already is. Film grain
// synthesis is the one piece that would move: it is embarrassingly parallel and
// a presenter is where it belongs, which is an argument for the split this ABI
// already makes. For now dav1d applies it, because a picture without the grain
// the stream asked for is a picture that is wrong.

#include <mediaperch/module.h>

#include <dav1d/dav1d.h>

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

/// The four fixed bytes of an AV1CodecConfigurationRecord, before the
/// configuration OBUs that follow them.
constexpr std::size_t k_av1c_fixed = 4;

/// The record's first byte: a marker bit of 1 and a version of 1.
constexpr std::uint8_t k_av1c_marker = 0x81u;

} // namespace

struct MpVideoCodec {
    ~MpVideoCodec()
    {
        if (have_picture) {
            dav1d_picture_unref(&picture);
        }
        if (pending.sz != 0) {
            dav1d_data_unref(&pending);
        }
        if (ctx != nullptr) {
            dav1d_close(&ctx);
        }
    }

    Dav1dContext* ctx = nullptr;

    /// The picture handed out by the last `next_frame`, kept alive because that
    /// is what the ABI promises -- valid until the next call on this codec.
    Dav1dPicture picture{};
    bool have_picture = false;

    /// **Input dav1d has not taken yet.** `dav1d_send_data` may consume part of
    /// a packet or none of it, and what is left stays the caller's until it
    /// does. Holding it here is what lets `decode` say MP_OK for a packet the
    /// decoder has not finished with.
    Dav1dData pending{};

    /// The configuration OBUs out of the `av1C` record, kept because a reset
    /// sends them again.
    std::string config;

    MpVideoInfo info{};
    bool have_format = false;
    std::string trouble;
};

namespace {

/// Whatever is pending, pushed as far as dav1d will take it.
///
/// EAGAIN is not an error: it means the decoder is holding frames and wants
/// `dav1d_get_picture` called before it will take more. Anything else is, and
/// the data is dropped rather than retried forever.
void push(MpVideoCodec* c) noexcept
{
    if (c->pending.sz == 0) {
        return;
    }
    const int r = dav1d_send_data(c->ctx, &c->pending);
    if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
        dav1d_data_unref(&c->pending);
    }
}

/// Copies bytes into a fresh `Dav1dData` and makes it the pending input.
bool take(MpVideoCodec* c, const void* bytes, std::size_t count, std::uint64_t pts) noexcept
{
    std::uint8_t* dst = dav1d_data_create(&c->pending, count);
    if (dst == nullptr) {
        return false;
    }
    std::memcpy(dst, bytes, count);
    // Passed through rather than interpreted: dav1d hands this back on the
    // picture that came of it, so the demuxer's timescale still holds. See
    // `codec_get_format`.
    c->pending.m.timestamp = static_cast<std::int64_t>(pts);
    return true;
}

/// The layout of a decoded picture, in the ABI's words.
///
/// Four chroma arrangements and three depths, and none of it is a table: dav1d
/// says which subsampling and how many bits, and the rest follows.
MpPixelLayout layout_of(const Dav1dPicture& p) noexcept
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    switch (p.p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400:
        out.chroma = MP_CHROMA_MONO;
        break;
    case DAV1D_PIXEL_LAYOUT_I422:
        out.chroma = MP_CHROMA_422;
        break;
    case DAV1D_PIXEL_LAYOUT_I444:
        out.chroma = MP_CHROMA_444;
        break;
    case DAV1D_PIXEL_LAYOUT_I420:
    default:
        out.chroma = MP_CHROMA_420;
        break;
    }
    out.packing = MP_PACK_PLANAR;
    out.bits = static_cast<std::uint32_t>(p.p.bpc);
    out.container_bits = p.p.bpc > 8 ? 16u : 8u;
    // **Zero, and it is the difference between this decoder and the hardware
    // one.** dav1d puts the significant bits in the low end of each word;
    // P010 puts them in the high end. The presenter reads `shift` rather than
    // guessing, and `mp_pixel_sample_scale` turns it into the right number.
    out.shift = 0;
    return out;
}

/// What the sequence header says, in the code points MpVideoInfo carries.
///
/// AV1 states ISO/IEC 23091-2 code points directly -- the same numbering
/// `MpVideoInfo` uses and the same one `demux_mp4` reads out of `colr` -- so
/// this is a copy rather than a mapping.
void read_sequence(MpVideoCodec* c, const Dav1dSequenceHeader& seq) noexcept
{
    const std::uint32_t size = c->info.size != 0 ? c->info.size : sizeof(MpVideoInfo);
    c->info = MpVideoInfo{};
    c->info.size = size;
    c->info.width = static_cast<std::uint32_t>(seq.max_width);
    c->info.height = static_cast<std::uint32_t>(seq.max_height);
    c->info.display_width = c->info.width;
    c->info.display_height = c->info.height;

    // **Only when the stream said so.** AV1 carries a
    // `color_description_present` flag, and a decoder that reported its
    // defaults as though the stream had stated them would out-rank the
    // container -- which is the mistake §9.8's join exists to prevent, in the
    // other direction. Unspecified is 2 in this numbering.
    if (seq.color_description_present) {
        c->info.primaries = static_cast<std::uint32_t>(seq.pri);
        c->info.transfer = static_cast<std::uint32_t>(seq.trc);
        c->info.matrix = static_cast<std::uint32_t>(seq.mtrx);
    } else {
        c->info.primaries = 2;
        c->info.transfer = 2;
        c->info.matrix = 2;
    }
    // Range is not part of the colour description and is always stated.
    c->info.flags = seq.color_range != 0 ? MP_VIDEO_FULL_RANGE : 0u;
    // **The one processing stage that is normative and separable.** dav1d
    // applies the grain by default, as every conformant decoder must, and says
    // here whether there is any -- which is what a presenter would read before
    // taking the synthesis onto the GPU, and what makes a cross-check against
    // another decoder meaningful on a grainy stream.
    if (seq.film_grain_present != 0) {
        c->info.flags |= MP_VIDEO_FILM_GRAIN;
    }

    // **Zero, and it means "unchanged" rather than "untimed" here.** dav1d
    // hands back the timestamp it was given, so whatever the demuxer counted
    // in still holds. A decoder that re-times -- Media Foundation does, in
    // hundred-nanosecond units -- states its own instead.
    c->info.timescale = 0;
    c->have_format = true;
}

void release_frame(MpVideoCodec* c) noexcept
{
    if (c->have_picture) {
        dav1d_picture_unref(&c->picture);
        c->picture = Dav1dPicture{};
        c->have_picture = false;
    }
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
    // An `av1C` that is too short to hold its own fixed fields, or whose first
    // byte is not the marker and version, is a record this module declines
    // rather than guesses at.
    if (config != nullptr && config_bytes != 0 &&
        (config_bytes < k_av1c_fixed || config[0] != k_av1c_marker)) {
        return MP_OK;
    }

    // **100 for system memory and 60 for a device**, and the gap is the
    // argument §7 makes for `codec_flac` over `codec_native`: this is a
    // decoder in the tree, checkable the way `codec_alac` is, rather than a
    // black box an operating system update can change under a hash. It decodes
    // just as well when a presenter has a device -- the frames simply arrive as
    // planes and are uploaded -- so it still claims that case, below whatever a
    // decoder that lands in a texture on that very device would score. Nothing
    // in this tree decodes AV1 on a GPU yet, so today 60 wins by default.
    *out_score = api == MP_GRAPHICS_NONE ? 100u : 60u;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    if (out == nullptr || codec != MP_CODEC_AV1) {
        return MP_ERR_INVALID;
    }
    // A device is accepted and unused: dav1d decodes on the CPU, and refusing
    // one would make a host with a presenter unable to decode AV1 at all.
    (void)device;

    auto c = std::unique_ptr<MpVideoCodec>(new (std::nothrow) MpVideoCodec());
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->info.size = sizeof(MpVideoInfo);

    Dav1dSettings settings{};
    dav1d_default_settings(&settings);
    // Zero means one thread per logical core, which is what a player wants and
    // what makes dav1d fast enough to be the answer for 4K.
    settings.n_threads = 0;
    if (dav1d_open(&c->ctx, &settings) != 0) {
        log_line(MP_LOG_ERROR, "codec_dav1d: dav1d would not open");
        return MP_ERR_UNSUPPORTED;
    }

    // **The configuration OBUs, which are the sequence header.** An `av1C`
    // record is four fixed bytes and then the OBUs a decoder needs before the
    // first frame -- so the fixed bytes are stepped over and the rest is sent
    // as data, exactly as it would be mid-stream. Nothing is rewritten: AV1
    // samples in an MP4 are already OBUs carrying their own sizes, which is the
    // whole difference from H.264's length-prefixed framing.
    if (config != nullptr && config_bytes > k_av1c_fixed) {
        if (config[0] != k_av1c_marker) {
            log_line(MP_LOG_DEBUG, "codec_dav1d: the av1C record has no marker byte");
            return MP_ERR_FORMAT;
        }
        c->config.assign(reinterpret_cast<const char*>(config) + k_av1c_fixed,
                         config_bytes - k_av1c_fixed);

        // Read before it is sent, so `get_format` answers before the first
        // frame rather than after it. A record with no sequence header in it
        // is not an error: the stream carries one in band.
        Dav1dSequenceHeader seq{};
        if (dav1d_parse_sequence_header(
                &seq, reinterpret_cast<const std::uint8_t*>(c->config.data()),
                c->config.size()) == 0) {
            read_sequence(c.get(), seq);
        }

        if (!take(c.get(), c->config.data(), c->config.size(), 0)) {
            return MP_ERR_NO_MEMORY;
        }
        push(c.get());
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
    // Whatever was left over goes first, and while any of it remains there is
    // nowhere to put a new packet -- which is what MP_ERR_BUSY says. The host
    // drains with `next_frame` and offers the packet again.
    push(c);
    if (c->pending.sz != 0) {
        return MP_ERR_BUSY;
    }
    if (!take(c, packet, bytes, pts)) {
        return MP_ERR_NO_MEMORY;
    }
    push(c);
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_next_frame(MpVideoCodec* c, MpVideoFrame* out) noexcept
try {
    if (c == nullptr || out == nullptr || out->size < sizeof(MpVideoFrame)) {
        return MP_ERR_INVALID;
    }
    release_frame(c);
    push(c);

    Dav1dPicture picture{};
    const int r = dav1d_get_picture(c->ctx, &picture);
    if (r == DAV1D_ERR(EAGAIN)) {
        return MP_END;
    }
    if (r < 0) {
        c->trouble = "dav1d could not decode this frame";
        return MP_ERR_FORMAT;
    }
    c->picture = picture;
    c->have_picture = true;

    if (picture.seq_hdr != nullptr) {
        read_sequence(c, *picture.seq_hdr);
        // The sequence header states the largest frame in the sequence; this
        // one states itself, and a stream whose frames are smaller than the
        // sequence allows is ordinary.
        c->info.width = static_cast<std::uint32_t>(picture.p.w);
        c->info.height = static_cast<std::uint32_t>(picture.p.h);
        c->info.display_width = c->info.width;
        c->info.display_height = c->info.height;
    }

    const std::uint32_t size = out->size;
    *out = MpVideoFrame{};
    out->size = size;
    out->width = static_cast<std::uint32_t>(picture.p.w);
    out->height = static_cast<std::uint32_t>(picture.p.h);
    out->layout = layout_of(picture);
    out->pts = static_cast<std::uint64_t>(picture.m.timestamp);

    // **One stride for luma and one for both chroma planes**, which is dav1d's
    // shape and not the ABI's -- so it is spread here rather than the ABI
    // growing a special case for one library.
    const std::uint32_t planes = mp_pixel_planes(&out->layout);
    out->plane[0] = picture.data[0];
    out->stride[0] = static_cast<std::uint32_t>(picture.stride[0]);
    for (std::uint32_t i = 1; i < planes; ++i) {
        out->plane[i] = picture.data[i];
        out->stride[i] = static_cast<std::uint32_t>(picture.stride[1]);
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
    // There is nothing to tell dav1d: it hands back what it is holding as soon
    // as no more data arrives, which is what `next_frame` returning MP_END
    // already means. All this owes is the last of the input.
    push(c);
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    release_frame(c);
    if (c->pending.sz != 0) {
        dav1d_data_unref(&c->pending);
    }
    dav1d_flush(c->ctx);

    // **The sequence header goes back in front, the way `codec_mft` puts the
    // parameter sets back.** An AV1 keyframe carries one in band and this is
    // very likely redundant -- which is the reason to do it rather than not:
    // a stream that does not, after a seek, is a picture that never arrives.
    if (!c->config.empty()) {
        if (!take(c, c->config.data(), c->config.size(), 0)) {
            return MP_ERR_NO_MEMORY;
        }
        push(c);
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

void MP_CALL module_shutdown(void) noexcept
{
    g_host = nullptr;
}

/// What it decodes, declared rather than asked -- see MpModuleDesc::codecs.
const MpCodec k_codecs[] = {MP_CODEC_AV1};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VCODEC,
    // **Above codec_mft's 80**, which is the same argument §7 makes for the
    // reference FLAC decoder over the single-header one: this is a decoder in
    // the tree that a test can hash. It never contends with codec_mft anyway,
    // because Media Foundation does not claim AV1 -- the number is what the
    // registry would use the day something else does.
    /* priority    */ 100,
    /* id          */ "codec_dav1d",
    /* name        */ "AV1, decoded by dav1d",
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

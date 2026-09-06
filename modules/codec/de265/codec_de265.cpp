// SPDX-License-Identifier: GPL-3.0-or-later
//
// HEVC, decoded by libde265.
//
// **This is the first HEVC decoder in this tree that is in this tree.** What
// there was before is `codec_mft`, which asks Media Foundation -- and on this
// machine the only thing that answered for HEVC was `HEVCVideoExtension`, the
// Store extension, which is absent from a clean Windows and cannot be shipped
// with anything. A player whose HEVC support depends on the user having
// installed something from a shop is a player that does not support HEVC.
//
// libde265 is LGPL-3.0 for the library (the sample applications are MIT), which
// is why it can be here at all and why it is a module of its own: this tree is
// GPL-3.0-or-later, LGPL-3.0 code may be linked into that, and §4's rule that
// nothing is allocated across the module boundary is what keeps the boundary a
// boundary rather than a formality.
//
// **What it does not do is as much of the shape as what it does.** libde265
// decodes Main profile -- eight-bit 4:2:0 -- and is incomplete above that, so
// `probe` reads the `hvcC`'s own chroma format and bit depths and declines
// anything else before a decoder is opened. §7's rule is that a decoder failing
// mid-file must never trigger a silent retry, and the way to keep it is to fail
// before the file starts.
//
// The pictures come out planar and are handed over as libde265's own memory,
// valid until the next call, which is what `MpVideoCodecVtbl` promises and
// what makes a 4K frame free rather than a copy.

#include "decoder_threads.hpp"
#include "h264.hpp"

#include <abi_guard.hpp>
#include <mediaperch/module.h>

#include <libde265/de265.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log_line(MpLogLevel level, const char* msg) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, msg);
    }
}

/// What libde265 will decode, which is narrower than what HEVC defines.
///
/// Main profile and nothing else: 4:2:0 at eight bits. The record states both,
/// so this is read rather than discovered.
[[nodiscard]] bool decodable(const mp::mft::HevcConfig& config) noexcept
{
    return config.valid && config.chroma_format_idc == 1 && config.bit_depth_luma == 8 &&
           config.bit_depth_chroma == 8;
}

} // namespace

struct MpVideoCodec {
    ~MpVideoCodec()
    {
        if (ctx != nullptr) {
            de265_free_decoder(ctx);
        }
    }

    de265_decoder_context* ctx = nullptr;

    /// The `hvcC`, kept because a reset has to push the parameter sets again.
    mp::mft::HevcConfig config;

    /// **The picture handed out by the last `next_frame`.** libde265 owns it
    /// and keeps it alive until it is released, which is what lets the ABI's
    /// "valid until the next call" be a pointer rather than a copy.
    const de265_image* image = nullptr;

    MpVideoInfo info{};
    bool have_format = false;
    std::string trouble;

    /// What a host asked for, and whether it is too late to ask.
    /// `modules/shared/decoder_threads` has the rules; what the number *means*
    /// is libde265's and stays beside the call that uses it.
    mp::ThreadChoice threads;
    /// **The pool starts at the first packet rather than at `open`**, which is
    /// what makes `set` mean anything here: libde265 takes its size once, in
    /// `de265_start_worker_threads`, and cannot resize it afterwards. Between
    /// `open` and the first `decode` there is nowhere to apply a number unless
    /// the start waits, so it waits.
    bool workers_started = false;
};

namespace {

/// Whatever libde265 is holding for us, handed back.
///
/// **Peek and release, not get.** `de265_get_next_picture` is `peek` followed
/// by `release` -- it takes the picture out of the queue and returns a pointer
/// into memory it no longer promises anything about. Pairing it with a release
/// of our own therefore releases the *next* picture, which is a no-op while the
/// queue is empty and throws one away when it is not. That is once per file, at
/// the end, where the queue finally holds something: 23 frames of 24, in the
/// right order, with nothing in any log to say what happened.
void release_frame(MpVideoCodec* c) noexcept
{
    if (c->image != nullptr) {
        de265_release_next_picture(c->ctx);
        c->image = nullptr;
    }
}

/// The layout of a picture, read from the picture rather than assumed.
[[nodiscard]] MpPixelLayout layout_of(const de265_image* image) noexcept
{
    MpPixelLayout layout{};
    layout.chroma = MP_CHROMA_420;
    layout.packing = MP_PACK_PLANAR;
    layout.bits = 8;
    layout.container_bits = 8;
    switch (de265_get_chroma_format(image)) {
    case de265_chroma_mono:
        layout.chroma = MP_CHROMA_MONO;
        break;
    case de265_chroma_422:
        layout.chroma = MP_CHROMA_422;
        break;
    case de265_chroma_444:
        layout.chroma = MP_CHROMA_444;
        break;
    default:
        break;
    }
    const int bits = de265_get_bits_per_pixel(image, 0);
    if (bits > 0) {
        layout.bits = static_cast<std::uint8_t>(bits);
        // Above eight bits libde265 stores two bytes a sample, little-endian,
        // with the value in the low bits -- which is `container_bits` of 16 and
        // a shift of zero, and is the same shape dav1d produces.
        layout.container_bits = bits > 8 ? 16 : 8;
    }
    return layout;
}

/// Pushes one complete NAL unit, without a start code.
///
/// **The container already delimits them, so nothing here re-delimits them.**
/// An MP4 sample is a list of NAL units each prefixed by its length, and
/// `de265_push_NAL` takes exactly that -- one unit, its own bytes, nothing
/// scanned for. The alternative is `de265_push_data`, which takes a byte
/// stream and finds the boundaries by looking for start codes, and it has one
/// property that matters here: a NAL is not complete until the *next* start
/// code arrives, so the last one of a file is still pending when the file ends.
/// Measured: 23 frames of 24, every time.
///
/// `de265_push_end_of_frame` fixes the count and breaks the order -- it sends
/// the picture straight to the output queue, so what comes back is decode
/// order and a stream with B-frames arrives shuffled. Giving libde265 the
/// boundaries it was going to look for is the answer that costs neither.
///
/// **The timestamp goes in with the unit**, which is the whole reason the call
/// takes one. HEVC reorders, so the timestamp of the last packet pushed is not
/// the timestamp of the next picture out; carrying it through is the only way
/// they stay together. Doing it the other way was measured too -- fourteen
/// frames of twenty-four dropped, the pacer having been told each picture was
/// due at the time of a packet several frames ahead of it.
[[nodiscard]] MpResult push_nal(MpVideoCodec* c, const std::uint8_t* nal, std::size_t bytes,
                                std::uint64_t pts)
{
    if (bytes < 2) {
        // The two-byte NAL header is the minimum libde265 accepts, and a unit
        // shorter than its own header is a truncated sample.
        c->trouble = "a NAL unit shorter than its header";
        return MP_ERR_FORMAT;
    }
    const de265_error pushed = de265_push_NAL(c->ctx, nal, static_cast<int>(bytes),
                                              static_cast<de265_PTS>(pts), nullptr);
    if (pushed != DE265_OK) {
        c->trouble = de265_get_error_text(pushed);
        return MP_ERR_FORMAT;
    }
    return MP_OK;
}

/// Every parameter set out of the `hvcC`, which a decoder needs before the
/// first sample and again after a reset.
[[nodiscard]] MpResult push_parameter_sets(MpVideoCodec* c)
{
    for (const std::vector<std::uint8_t>& nal : c->config.annex.parameter_sets) {
        const MpResult r = push_nal(c, nal.data(), nal.size(), 0);
        if (r != MP_OK) {
            return r;
        }
    }
    return MP_OK;
}

/// Every NAL unit of one sample, in order.
///
/// False for a sample that is not the length-prefixed list the `hvcC` says it
/// is: a length running past the end is a truncated file, and pushing what is
/// left would hand the decoder a slice that stops in the middle.
[[nodiscard]] MpResult push_sample(MpVideoCodec* c, const std::uint8_t* sample,
                                   std::size_t bytes, std::uint64_t pts)
{
    const std::uint32_t length_size = c->config.annex.length_size;
    std::size_t at = 0;
    while (at < bytes) {
        if (bytes - at < length_size) {
            c->trouble = "a NAL length that does not fit in the sample";
            return MP_ERR_FORMAT;
        }
        std::uint64_t length = 0;
        for (std::uint32_t i = 0; i < length_size; ++i) {
            length = (length << 8) | sample[at + i];
        }
        at += length_size;
        if (length > bytes - at) {
            c->trouble = "a NAL unit that runs past the end of the sample";
            return MP_ERR_FORMAT;
        }
        if (length != 0) {
            const MpResult r = push_nal(c, sample + at, static_cast<std::size_t>(length),
                                        pts);
            if (r != MP_OK) {
                return r;
            }
        }
        at += static_cast<std::size_t>(length);
    }
    return MP_OK;
}

MpResult MP_CALL codec_probe(MpCodec codec, MpGraphicsApi api, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
try {
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_HEVC) {
        return MP_ERR_UNSUPPORTED;
    }
    if (config == nullptr || config_bytes == 0) {
        // **An HEVC stream with no `hvcC` is one this cannot start.** The
        // parameter sets could arrive in band, and libde265 would find them --
        // but nothing here could have declined a ten-bit file first, and a
        // decoder that discovers halfway through that it cannot do this is
        // exactly what §7 forbids.
        return MP_ERR_UNSUPPORTED;
    }
    const mp::mft::HevcConfig parsed = mp::mft::parse_hvcc(config, config_bytes);
    if (!decodable(parsed)) {
        return MP_ERR_UNSUPPORTED;
    }
    // **80 for system memory and 50 for a device**, which is dav1d's shape and
    // the same reasoning: this decodes just as well when the presenter has a
    // device -- the frames arrive as planes and are uploaded -- so it claims
    // that case too, below whatever lands in a texture on that very device.
    // What it does not claim is that it is better than a working hardware
    // decoder, because it is not.
    *out_score = api == MP_GRAPHICS_NONE ? 80u : 50u;
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    if (codec != MP_CODEC_HEVC) {
        return MP_ERR_UNSUPPORTED;
    }
    // A device is accepted and ignored: the frames are planes in system
    // memory either way, and the host uploads them. Saying no here would refuse
    // the case `probe` just claimed at 50.
    (void)device;
    if (config == nullptr || config_bytes == 0) {
        return MP_ERR_UNSUPPORTED;
    }

    auto owned = std::make_unique<MpVideoCodec>();
    owned->config = mp::mft::parse_hvcc(config, config_bytes);
    if (!decodable(owned->config)) {
        log_line(MP_LOG_DEBUG, "codec_de265: the hvcC is not 8-bit 4:2:0");
        return MP_ERR_UNSUPPORTED;
    }

    owned->ctx = de265_new_decoder();
    if (owned->ctx == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    // **Worker threads, and the comment that used to be here was wrong twice
    // over.** It said one thread was deliberate because libde265's workers
    // "decode frames ahead of the one asked for", which is not what they do --
    // the header says they parallelise WPP and tile decoding *inside* one
    // picture and do not relax the rule that one thread owns the context. And
    // it was attached to two `set_parameter` calls that set defaults, so
    // nothing was configuring any threading at all. A rationale nobody checked,
    // for code that did not do what it claimed.
    //
    // 4K found it: three seconds of 3840x2160 decoded on one core, the video
    // fell far enough behind that 34 of 71 frames were dropped, and the audio
    // device underran 173 times -- which is M6's acceptance condition failing
    // on the half that says the picture never wins.
    //
    // `mp::decoder_threads()` is this tree's answer everywhere else, and
    // libde265 caps its own pool at 32 (MAX_THREADS in threads.h), so the
    // smaller of the two is what it is asked for. A failure here is not fatal:
    // the decoder still works on the calling thread, which is what it did
    // before this line existed.

    // The parameter sets, before any sample.
    const MpResult sets = push_parameter_sets(owned.get());
    if (sets != MP_OK) {
        log_line(MP_LOG_ERROR, ("codec_de265: " + owned->trouble).c_str());
        return sets;
    }

    *out = owned.release();
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

void MP_CALL codec_close(MpVideoCodec* c) noexcept
{
    if (c != nullptr) {
        release_frame(c);
    }
    delete c;
}

MpResult MP_CALL codec_get_format(MpVideoCodec* c, MpVideoInfo* out) noexcept
try {
    if (c == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    if (!c->have_format) {
        // Nothing has been decoded, so the container's description is the only
        // one there is -- which is what MP_ERR_BUSY means here.
        return MP_ERR_BUSY;
    }
    const std::uint32_t size = out->size;
    *out = c->info;
    out->size = size;
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

/// Starts the pool the first time a packet arrives, and never again.
///
/// A failure is not fatal: the decoder still works on the calling thread, which
/// is what it did before there were any workers at all.
void start_workers(MpVideoCodec* c) noexcept
{
    c->threads.fix();
    if (c->workers_started) {
        return;
    }
    c->workers_started = true;
    // What was asked for, or this machine's cores when nobody asked. libde265
    // caps its own pool at 32 (MAX_THREADS in threads.h), so more than that is
    // the same answer rather than a bigger one.
    const unsigned want =
        c->threads.chosen() != 0 ? c->threads.chosen() : mp::decoder_threads();
    const unsigned threads = std::min<unsigned>(want, 32u);
    if (threads <= 1) {
        return;
    }
    const de265_error started =
        de265_start_worker_threads(c->ctx, static_cast<int>(threads));
    if (started != DE265_OK) {
        log_line(MP_LOG_WARN, (std::string{"codec_de265: no worker threads: "} +
                               de265_get_error_text(started))
                                  .c_str());
    }
}

/// `threads`, and nothing else yet.
///
/// **Refused once the pool exists**, with MP_ERR_BUSY, rather than accepted and
/// ignored: libde265 sizes its pool once and a host told "yes" to a number that
/// will not be used would measure the old one and write down the new one.
MpResult MP_CALL codec_set(MpVideoCodec* c, const char* key, const char* value) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    return c->threads.take(key, value, c->trouble);
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL codec_decode(MpVideoCodec* c, const void* packet, std::size_t bytes,
                              std::uint64_t pts) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    start_workers(c);
    if (packet == nullptr || bytes == 0) {
        return MP_OK;
    }
    // **The parameter sets go in front of every packet, not just the first.**
    // A seek resets the decoder and a reset forgets them; sending them again
    // costs a few dozen bytes and removes a class of failure that only appears
    // after somebody drags a slider.
    const MpResult sets = push_parameter_sets(c);
    if (sets != MP_OK) {
        return sets;
    }
    return push_sample(c, static_cast<const std::uint8_t*>(packet), bytes, pts);
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL codec_next_frame(MpVideoCodec* c, MpVideoFrame* out) noexcept
try {
    if (c == nullptr || out == nullptr || out->size < sizeof(MpVideoFrame)) {
        return MP_ERR_INVALID;
    }
    release_frame(c);

    // Decode until a picture appears or libde265 runs out of work. `more` is
    // its own answer to "is there anything left to do", and looping on it is
    // how a decoder that needs several NALs for one picture gets them.
    //
    // **A picture is asked for on every path out of the loop, including the
    // ones that are not success.** `de265_decode` says
    // DE265_ERROR_WAITING_FOR_INPUT_DATA when it wants more bytes -- and after
    // `flush` there are none and never will be, while the last picture of the
    // file is sitting in the reorder buffer waiting to be collected. Returning
    // MP_END on that code lost exactly one frame per file: 23 of 24, every
    // time, with no warning from libde265 because nothing was wrong.
    int more = 1;
    while (more != 0 && c->image == nullptr) {
        const de265_error err = de265_decode(c->ctx, &more);
        if (err != DE265_OK) {
            if (err != DE265_ERROR_WAITING_FOR_INPUT_DATA && !de265_isOK(err)) {
                c->trouble = de265_get_error_text(err);
                return MP_ERR_FORMAT;
            }
            more = 0; // nothing further to decode; something may still be out
        }
        // **The warnings, drained and said.** libde265 reports a picture it
        // could not build, a checksum that did not match and a reference it
        // never saw as warnings rather than errors, and a decoder that throws
        // them away is a decoder whose missing frame has no explanation.
        for (;;) {
            const de265_error warning = de265_get_warning(c->ctx);
            if (warning == DE265_OK) {
                break;
            }
            log_line(MP_LOG_WARN,
                     (std::string{"codec_de265: "} + de265_get_error_text(warning)).c_str());
        }
        // Peeked rather than taken: it stays in the queue, and therefore
        // alive, until the next call releases it. See release_frame.
        c->image = de265_peek_next_picture(c->ctx);
    }
    if (c->image == nullptr) {
        return MP_END;
    }

    const de265_image* image = c->image;
    c->info.width = static_cast<std::uint32_t>(de265_get_image_width(image, 0));
    c->info.height = static_cast<std::uint32_t>(de265_get_image_height(image, 0));
    c->info.display_width = c->info.width;
    c->info.display_height = c->info.height;
    c->have_format = true;

    const std::uint32_t size = out->size;
    *out = MpVideoFrame{};
    out->size = size;
    out->width = c->info.width;
    out->height = c->info.height;
    out->layout = layout_of(image);
    // Read off the picture, which is where it has been since `decode` pushed
    // it with the bytes.
    out->pts = static_cast<std::uint64_t>(de265_get_image_PTS(image));

    const std::uint32_t planes = mp_pixel_planes(&out->layout);
    for (std::uint32_t i = 0; i < planes; ++i) {
        int stride = 0;
        out->plane[i] = de265_get_image_plane(image, static_cast<int>(i), &stride);
        out->stride[i] = static_cast<std::uint32_t>(stride);
    }
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL codec_flush(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // The end of the input, which is what makes libde265 hand back the pictures
    // it was holding for reordering rather than waiting for more.
    de265_flush_data(c->ctx);
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
try {
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    release_frame(c);
    de265_reset(c->ctx);
    // Reset forgets the parameter sets, and every packet carries them anyway --
    // but the next packet might be a non-keyframe on a path that does not, so
    // they go back in here as well.
    return push_parameter_sets(c);
}
MEDIAPERCH_ABI_GUARD_CATCH

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
    /* set        */ &codec_set,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
try {
    g_host = host;
    log_line(MP_LOG_DEBUG, "codec_de265: libde265 " LIBDE265_VERSION);
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

void MP_CALL module_shutdown(void) noexcept
{
    g_host = nullptr;
}

const MpCodec k_codecs[] = {MP_CODEC_HEVC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(1, 1, 2),
    /* kind        */ MP_KIND_VCODEC,
    // **Below codec_mft's 80 and above nothing.** Media Foundation decodes the
    // same stream without the CPU where it can, and on a machine where it can
    // it should. What this is for is the machine where it cannot -- which on
    // the one this was written on is every machine without a Store extension
    // installed, and that is most of them.
    /* priority    */ 60,
    /* id          */ "codec_de265",
    /* name        */ "HEVC, decoded by libde265",
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
    return host_abi == MP_ABI_VERSION ? &g_desc : nullptr;
}

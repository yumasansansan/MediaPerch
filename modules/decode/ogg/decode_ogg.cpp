// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vorbis and Opus, through libvorbis/vorbisfile and libopus/opusfile: Xiph's
// own libraries, as submodules.
//
// Two things are worth saying up front, because both are decisions rather than
// details.
//
// **These decoders output float, and this module says so.** libvorbis is a
// float codec internally and `ov_read` only reaches int16 by converting;
// likewise `op_read` against a float build of libopus. A decoder in this tree
// never converts -- that is the graph's job, on Path B, where it is visible --
// so this module calls `ov_read_float` and `op_read_float` and reports
// MP_SAMPLE_F32. The consequence is honest and slightly inconvenient: a lossy
// file cannot take Path A. It never could. There is no bit-exact rendering of a
// codec whose output is defined as a floating-point signal, and a module that
// quietly rounded to S32 to look like it belonged on Path A would be claiming
// an exactness that exists nowhere in the chain.
//
// **The channel order is permuted, and only the order.** Ogg's channel layouts
// are Vorbis's, and Opus mapping family 1 is defined to be the same; Windows
// wants WAVE order. For anything past stereo the two disagree -- Vorbis 5.1 is
// L,C,R,BL,BR,LFE and WAVE 5.1 is L,R,C,LFE,BL,BR -- so a decoder that passed
// the samples straight through would put the centre channel in the right
// speaker. The permutation below moves samples between slots and never touches
// their values, which is why it is allowed to live in a decoder at all.

#include <mediaperch/module.h>

#include <ogg/ogg.h>
#include <opusfile.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log(MpLogLevel level, const char* message) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, message);
    }
}

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log(level, buffer);
}

// ---------------------------------------------------------------- channels

// For each output slot in WAVE order, the index of the Ogg channel that belongs
// there. Rows 1 and 2 are the identity: mono and stereo agree between the two
// conventions, and reporting no mask for them matches what the other decoders
// in this tree do.
//
// The 7-channel row is the one to read twice. Vorbis 6.1 is L,C,R,SL,SR,BC,LFE,
// so LFE is *last* there and fourth in WAVE.
struct Layout {
    std::uint32_t mask;
    int from[8];
};

const Layout k_layouts[9] = {
    /* 0 */ {0u, {0, 0, 0, 0, 0, 0, 0, 0}},
    /* 1 */ {0u, {0, 0, 0, 0, 0, 0, 0, 0}},
    /* 2 */ {0u, {0, 1, 0, 0, 0, 0, 0, 0}},
    /* 3: L C R              */ {0x1u | 0x2u | 0x4u, {0, 2, 1, 0, 0, 0, 0, 0}},
    /* 4: L R BL BR          */ {0x1u | 0x2u | 0x10u | 0x20u, {0, 1, 2, 3, 0, 0, 0, 0}},
    /* 5: L C R BL BR        */ {0x1u | 0x2u | 0x4u | 0x10u | 0x20u, {0, 2, 1, 3, 4, 0, 0, 0}},
    /* 6: L C R BL BR LFE    */
    {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u, {0, 2, 1, 5, 3, 4, 0, 0}},
    /* 7: L C R SL SR BC LFE */
    {0x1u | 0x2u | 0x4u | 0x8u | 0x100u | 0x200u | 0x400u, {0, 2, 1, 6, 5, 3, 4, 0}},
    /* 8: L C R SL SR BL BR LFE */
    {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x200u | 0x400u, {0, 2, 1, 7, 5, 6, 3, 4}},
};

bool needs_permute(std::uint32_t channels) noexcept
{
    if (channels < 3u || channels > 8u) {
        return false;
    }
    const Layout& l = k_layouts[channels];
    for (std::uint32_t c = 0; c < channels; ++c) {
        if (l.from[c] != static_cast<int>(c)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- callbacks

enum class Codec { vorbis, opus };

// opusfile is handed a FILE* through these rather than through op_fopen,
// because op_fopen takes a narrow path and this player has to open files whose
// names are not representable in the active code page.
int op_cb_read(void* stream, unsigned char* ptr, int nbytes) noexcept
{
    if (nbytes < 0) {
        return -1;
    }
    FILE* fp = static_cast<FILE*>(stream);
    const std::size_t got = std::fread(ptr, 1, static_cast<std::size_t>(nbytes), fp);
    if (got == 0 && std::ferror(fp) != 0) {
        return -1;
    }
    return static_cast<int>(got);
}

int op_cb_seek(void* stream, opus_int64 offset, int whence) noexcept
{
    return _fseeki64(static_cast<FILE*>(stream), offset, whence);
}

opus_int64 op_cb_tell(void* stream) noexcept
{
    return _ftelli64(static_cast<FILE*>(stream));
}

int op_cb_close(void* stream) noexcept
{
    return std::fclose(static_cast<FILE*>(stream));
}

const OpusFileCallbacks k_op_callbacks = {&op_cb_read, &op_cb_seek, &op_cb_tell, &op_cb_close};

} // namespace

struct MpDecoder {
    Codec codec = Codec::vorbis;
    OggVorbis_File vf{};
    bool vf_open = false;
    OggOpusFile* of = nullptr;
    MpFormat format{};
    std::uint64_t length = 0;
    bool permute = false;
    // Only allocated when the channel order actually has to change.
    std::vector<float> scratch;
    std::string path;
};

namespace {

FILE* open_utf8(const char* path) noexcept
{
#if defined(_WIN32)
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wide_len <= 0) {
        return nullptr;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_len));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), wide_len);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, wide.data(), L"rb") != 0) {
        return nullptr;
    }
    return fp;
#else
    return std::fopen(path, "rb");
#endif
}

/// Copies `frames` interleaved frames from `src` to `dst`, moving each channel
/// to its WAVE slot. Values are copied, never combined.
void permute_frames(float* dst, const float* src, std::size_t frames,
                    std::uint32_t channels) noexcept
{
    const int* from = k_layouts[channels].from;
    for (std::size_t f = 0; f < frames; ++f) {
        const float* in = src + f * channels;
        float* out = dst + f * channels;
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[c] = in[from[c]];
        }
    }
}

std::size_t frame_bytes(const MpFormat& f) noexcept
{
    return static_cast<std::size_t>(f.channels) * sizeof(float);
}

/// The teardown both decoder_close and a failed open need, in one place so the
/// two cannot drift apart.
void decoder_close_impl(MpDecoder* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->of != nullptr) {
        op_free(d->of);
    }
    if (d->vf_open) {
        ov_clear(&d->vf);
    }
    delete d;
}

// ---------------------------------------------------------------- vtable

MpResult MP_CALL decoder_probe(const char* path, const std::uint8_t* head, std::size_t head_bytes,
                               std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || head_bytes < 32) {
        return MP_OK;
    }
    if (std::memcmp(head, "OggS", 4) != 0) {
        return MP_OK;
    }
    // The codec identification header is in the first page, so a kilobyte is
    // always enough to tell Vorbis from Opus from everything else.
    const std::size_t scan = head_bytes < 1024 ? head_bytes : 1024;
    for (std::size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(head + i, "OpusHead", 8) == 0) {
            *out_score = 100;
            return MP_OK;
        }
    }
    for (std::size_t i = 0; i + 7 <= scan; ++i) {
        if (head[i] == 0x01 && std::memcmp(head + i + 1, "vorbis", 6) == 0) {
            *out_score = 100;
            return MP_OK;
        }
    }
    // An Ogg stream this module does not decode -- OggFLAC, Speex, Theora.
    // Score 0 rather than something low: it is not that we are worse at these,
    // it is that we cannot read them at all.
    return MP_OK;
}

MpResult finish_open(MpDecoder* d) noexcept
{
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;

    if (d->codec == Codec::opus) {
        // Opus decodes at 48 kHz by definition, whatever the source was sampled
        // at. The number is not negotiable and is not reported per link.
        rate = 48000u;
        const int ch = op_channel_count(d->of, -1);
        if (ch <= 0) {
            return MP_ERR_FORMAT;
        }
        channels = static_cast<std::uint32_t>(ch);

        const int links = op_link_count(d->of);
        for (int li = 0; li < links; ++li) {
            if (op_channel_count(d->of, li) != ch) {
                log_fmt(MP_LOG_ERROR,
                        "%s is a chained Opus stream whose links disagree about channel "
                        "count; refusing rather than changing layout halfway through",
                        d->path.c_str());
                return MP_ERR_FORMAT;
            }
        }

        const ogg_int64_t total = op_pcm_total(d->of, -1);
        d->length = total > 0 ? static_cast<std::uint64_t>(total) : 0;
    } else {
        const vorbis_info* vi = ov_info(&d->vf, -1);
        if (vi == nullptr || vi->rate <= 0 || vi->channels <= 0) {
            return MP_ERR_FORMAT;
        }
        rate = static_cast<std::uint32_t>(vi->rate);
        channels = static_cast<std::uint32_t>(vi->channels);

        const long links = ov_streams(&d->vf);
        for (long li = 0; li < links; ++li) {
            const vorbis_info* link = ov_info(&d->vf, static_cast<int>(li));
            if (link == nullptr || link->rate != vi->rate || link->channels != vi->channels) {
                log_fmt(MP_LOG_ERROR,
                        "%s is a chained Vorbis stream whose links disagree about format; "
                        "refusing rather than changing format mid-read",
                        d->path.c_str());
                return MP_ERR_FORMAT;
            }
        }

        const ogg_int64_t total = ov_pcm_total(&d->vf, -1);
        d->length = total > 0 ? static_cast<std::uint64_t>(total) : 0;
    }

    if (channels == 0u || channels > 8u) {
        log_fmt(MP_LOG_ERROR, "%s has %u channels, which this module has no WAVE layout for",
                d->path.c_str(), channels);
        return MP_ERR_FORMAT;
    }

    d->format.sample_rate = rate;
    d->format.channels = channels;
    d->format.channel_mask = k_layouts[channels].mask;
    d->format.sample_type = MP_SAMPLE_F32;
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = 32;
    d->permute = needs_permute(channels);
    return MP_OK;
}

MpResult MP_CALL decoder_open(const char* path, MpDecoder** out) noexcept
{
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    FILE* fp = open_utf8(path);
    if (fp == nullptr) {
        return MP_ERR_IO;
    }

    MpDecoder* d = new (std::nothrow) MpDecoder();
    if (d == nullptr) {
        std::fclose(fp);
        return MP_ERR_NO_MEMORY;
    }
    d->path = path;

    // Try Opus first, then Vorbis. Asking the libraries beats parsing the Ogg
    // page here: each already knows exactly what its own identification header
    // looks like, and neither accepts a file that belongs to the other.
    int err = 0;
    d->of = op_open_callbacks(fp, &k_op_callbacks, nullptr, 0, &err);
    if (d->of != nullptr) {
        // opusfile owns the FILE* from here and closes it in op_free.
        d->codec = Codec::opus;
    } else {
        // On failure op_open_callbacks leaves the stream open by contract, so
        // the same FILE* is ours to rewind and hand to vorbisfile.
        if (_fseeki64(fp, 0, SEEK_SET) != 0) {
            std::fclose(fp);
            delete d;
            return MP_ERR_IO;
        }
        if (ov_open_callbacks(fp, &d->vf, nullptr, 0, OV_CALLBACKS_DEFAULT) != 0) {
            std::fclose(fp);
            delete d;
            return MP_ERR_FORMAT;
        }
        d->vf_open = true;
        d->codec = Codec::vorbis;
    }

    const MpResult r = finish_open(d);
    if (r != MP_OK) {
        decoder_close_impl(d);
        return r;
    }

    *out = d;
    return MP_OK;
}

MpResult MP_CALL decoder_get_format(MpDecoder* d, MpFormat* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = d->format;
    return MP_OK;
}

MpResult MP_CALL decoder_get_length(MpDecoder* d, std::uint64_t* out_frames) noexcept
{
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = d->length;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
{
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t fb = frame_bytes(d->format);
    const std::size_t want_frames = fb == 0 ? 0 : dst_bytes / fb;
    if (want_frames == 0) {
        return MP_OK;
    }

    const std::uint32_t channels = d->format.channels;
    float* const dest = static_cast<float*>(dst);

    // Both libraries return at most one packet's worth per call, so a single
    // call rarely fills a buffer. Looping here keeps that fact out of the graph.
    std::size_t done = 0;
    while (done < want_frames) {
        const std::size_t room = want_frames - done;
        long got = 0;

        if (d->codec == Codec::opus) {
            float* target = dest + done * channels;
            if (d->permute) {
                if (d->scratch.size() < room * channels) {
                    d->scratch.resize(room * channels);
                }
                target = d->scratch.data();
            }
            // op_read_float counts floats, not frames, and takes an int.
            std::size_t floats = room * channels;
            if (floats > 0x100000u) {
                floats = 0x100000u;
            }
            const int n = op_read_float(d->of, target, static_cast<int>(floats), nullptr);
            if (n < 0) {
                log_fmt(MP_LOG_ERROR, "opusfile: error %d reading %s", n, d->path.c_str());
                return MP_ERR_IO;
            }
            if (n == 0) {
                break;
            }
            if (d->permute) {
                permute_frames(dest + done * channels, d->scratch.data(),
                               static_cast<std::size_t>(n), channels);
            }
            got = n;
        } else {
            // ov_read_float hands back planar pointers into libvorbis's own
            // buffers, so the interleave happens here either way and the
            // permutation costs nothing on top of it.
            float** pcm = nullptr;
            const long n = ov_read_float(&d->vf, &pcm, static_cast<int>(room), nullptr);
            if (n < 0) {
                log_fmt(MP_LOG_ERROR, "vorbisfile: error %ld reading %s", n, d->path.c_str());
                return MP_ERR_IO;
            }
            if (n == 0) {
                break;
            }
            const int* from = k_layouts[channels].from;
            float* out = dest + done * channels;
            for (std::uint32_t c = 0; c < channels; ++c) {
                const float* src = pcm[d->permute ? static_cast<std::uint32_t>(from[c]) : c];
                for (long f = 0; f < n; ++f) {
                    out[static_cast<std::size_t>(f) * channels + c] = src[f];
                }
            }
            got = n;
        }

        done += static_cast<std::size_t>(got);
    }

    *out_bytes = done * fb;
    return done == 0 ? MP_END : MP_OK;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    const ogg_int64_t target = static_cast<ogg_int64_t>(frame);
    if (d->codec == Codec::opus) {
        return op_pcm_seek(d->of, target) == 0 ? MP_OK : MP_ERR_IO;
    }
    return ov_pcm_seek(&d->vf, target) == 0 ? MP_OK : MP_ERR_IO;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    decoder_close_impl(d);
}

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpDecoderVtbl g_decoder_vtbl = {
    /* size       */ sizeof(MpDecoderVtbl),
    /* reserved   */ 0,
    /* probe      */ &decoder_probe,
    /* open       */ &decoder_open,
    /* get_format */ &decoder_get_format,
    /* get_length */ &decoder_get_length,
    /* read       */ &decoder_read,
    /* seek       */ &decoder_seek,
    /* close      */ &decoder_close,
};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DECODER,
    /* priority    */ 110, // the reference decoders, above decode_mf and decode_ffmpeg
    /* id          */ "decode_ogg",
    /* name        */ "Vorbis and Opus (libvorbis and libopus, the reference decoders)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_decoder_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi_version)
{
    if (host_abi_version != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

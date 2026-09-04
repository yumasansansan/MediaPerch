// SPDX-License-Identifier: GPL-3.0-or-later
//
// WavPack, on libwavpack -- and the module that has to explain why it is one
// module rather than two.
//
// **libwavpack has no block-level API, and that is the whole design decision
// here.** `demux_flac` splits the container from the codec because libFLAC
// documents `FLAC__stream_decoder_skip_single_frame` and
// `get_decode_position` for exactly that; libwavpack has nothing analogous.
// `WavpackVerifySingleBlock` checks a block's integrity and there is no way to
// ask where one begins, to skip one, or to decode one on its own. The public
// API abstracts blocks away and hands back samples.
//
// So there are two honest shapes and this is the second:
//
//   * flag the stream `MP_STREAM_SELF_DECODES` and answer `read_frames`, which
//     is what `demux_mf` and `demux_ffmpeg` do. §7 of the plan calls that the
//     floor, and it is: it is a second path through the host, it bypasses the
//     codec resolution the whole of v2 was built for, and it exists for the
//     two modules that are whole pipelines somebody else wrote.
//   * **name the codec the payload actually is, and hand it packets** -- which
//     is what `demux_wav` does. WavPack's payload after libwavpack has read it
//     is PCM, so the codec is `codec_pcm` and the "decode" is the memcpy it
//     always was; and when the file is a DSD one it is DSD, so the codec is
//     `codec_dsd` and the packets are the file's own bits.
//
// The second costs one buffer and keeps every host path the same -- seeking,
// gapless, the position, the format matrix, Path A's repack. It also means a
// DSD WavPack file reaches a DAC as DoP through exactly the code a .dsf does,
// which is the thing that would have been reimplemented under the first shape.
//
// **`OPEN_DSD_NATIVE`, never `OPEN_DSD_AS_PCM`.** libwavpack offers both, and
// the second decimates DSD by eight into 24-bit PCM inside the library. That is
// a conversion performed where nobody can see it, which is the objection this
// tree made to libxaac's integer-only AAC output and to Media Foundation's
// resampling, and it would be no better for being convenient.

#include <mediaperch/module.h>

#include <wavpack.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    if (g_host == nullptr || g_host->log == nullptr) {
        return;
    }
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host->log(g_host->ctx, level, buffer);
}

// **The file is opened here, not by libwavpack.**
//
// `WavpackOpenFileInput` takes a `char*` path, and on Windows that is the ANSI
// code page unless `OPEN_FILE_UTF8` is passed -- a flag whose behaviour is one
// more thing to be right about, on a path this tree already knows how to open.
// `WavpackOpenFileInputEx64` takes callbacks instead, so the host's UTF-8 path
// goes through the same `_wfopen_s` every other module here uses and libwavpack
// never sees a filename at all. The same arrangement `demux_mpa` has with
// libmpg123 and `demux_mp4` with Bento4.
#if defined(_WIN32)
FILE* open_utf8(const char* path)
{
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (len <= 1) {
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), len);
    FILE* fp = nullptr;
    return ::_wfopen_s(&fp, wide.c_str(), L"rb") == 0 ? fp : nullptr;
}
#else
FILE* open_utf8(const char* path) { return std::fopen(path, "rb"); }
#endif

/// A `FILE*` with one byte of push-back, which is what libwavpack's reader
/// interface asks for and what `ungetc` cannot promise more than one of.
struct Stream {
    FILE* fp = nullptr;
    int pushed = -1; ///< the pushed-back byte, or -1
};

std::int32_t io_read(void* id, void* data, std::int32_t bytes)
{
    auto* s = static_cast<Stream*>(id);
    if (s->fp == nullptr || bytes <= 0) {
        return 0;
    }
    auto* out = static_cast<std::uint8_t*>(data);
    std::int32_t got = 0;
    if (s->pushed >= 0) {
        *out++ = static_cast<std::uint8_t>(s->pushed);
        s->pushed = -1;
        got = 1;
        if (bytes == 1) {
            return 1;
        }
    }
    return got + static_cast<std::int32_t>(
                     std::fread(out, 1, static_cast<std::size_t>(bytes - got), s->fp));
}

std::int32_t io_write(void*, void*, std::int32_t)
{
    return 0; // tag editing only, and this module never writes
}

std::int64_t io_get_pos(void* id)
{
    auto* s = static_cast<Stream*>(id);
    const std::int64_t at = _ftelli64(s->fp);
    return s->pushed >= 0 ? at - 1 : at;
}

int io_set_pos_abs(void* id, std::int64_t pos)
{
    auto* s = static_cast<Stream*>(id);
    s->pushed = -1;
    return _fseeki64(s->fp, pos, SEEK_SET);
}

int io_set_pos_rel(void* id, std::int64_t delta, int mode)
{
    auto* s = static_cast<Stream*>(id);
    if (s->pushed >= 0 && mode == SEEK_CUR) {
        delta -= 1; // the pushed byte is one the caller has not consumed
    }
    s->pushed = -1;
    return _fseeki64(s->fp, delta, mode);
}

int io_push_back(void* id, int c)
{
    static_cast<Stream*>(id)->pushed = c;
    return c;
}

std::int64_t io_get_length(void* id)
{
    auto* s = static_cast<Stream*>(id);
    const std::int64_t at = _ftelli64(s->fp);
    if (_fseeki64(s->fp, 0, SEEK_END) != 0) {
        return 0;
    }
    const std::int64_t end = _ftelli64(s->fp);
    (void)_fseeki64(s->fp, at, SEEK_SET);
    return end;
}

int io_can_seek(void*) { return 1; }
int io_truncate(void*) { return -1; }
int io_close(void*) { return 0; } // the FILE* is this module's to close

WavpackStreamReader64 g_reader = {
    &io_read,     &io_write,   &io_get_pos,    &io_set_pos_abs, &io_set_pos_rel,
    &io_push_back, &io_get_length, &io_can_seek, &io_truncate,   &io_close,
};

/// Frames per packet. The same order of magnitude as every other demuxer here,
/// and a whole number of DoP frames so a DSD file's packets are even -- which
/// `codec_dsd` needs and `demux_dsd` guarantees the same way.
constexpr std::uint32_t k_packet_frames = 4096;

MpSampleType sample_type_for(unsigned container, unsigned valid) noexcept
{
    switch (container) {
    case 2: return valid <= 16 ? MP_SAMPLE_S16 : MP_SAMPLE_NONE;
    case 3: return valid <= 24 ? MP_SAMPLE_S24_PACKED : MP_SAMPLE_NONE;
    case 4: return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    default: return MP_SAMPLE_NONE;
    }
}

} // namespace

struct MpDemux {
    Stream stream;
    WavpackContext* wpc = nullptr;
    MpFormat format{};
    MpCodec codec = MP_CODEC_PCM;
    /// Bytes one frame takes on the wire this module produces.
    std::uint32_t frame_bytes = 0;
    /// Bytes libwavpack's own container takes: it always unpacks into `int32`.
    unsigned container = 0;
    bool is_dsd = false;
    bool is_float = false;
    std::uint64_t total_frames = 0;
    std::uint64_t position = 0;
    /// `k_packet_frames` frames of `int32` per channel, as libwavpack hands
    /// them over, before they are narrowed into the wire container.
    std::vector<std::int32_t> unpacked;
    /// A DSD file's configuration for `codec_dsd`: nine bytes, and the same
    /// nine `demux_dsd` assembles.
    std::uint8_t config[9] = {};
    std::uint32_t config_bytes = 0;
};

namespace {

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                             std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // `wvpk` at offset zero, and nothing else claims those four bytes. A
    // WavPack file is a run of blocks and the first one starts the file, so
    // unlike the frame-header formats there is nothing to confirm with a second
    // look.
    *out_score = (head != nullptr && bytes >= 4 && std::memcmp(head, "wvpk", 4) == 0) ? 100u : 0u;
    return MP_OK;
}

MpResult MP_CALL demux_open(const char* path, MpDemux** out) noexcept
try {
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    auto* d = new (std::nothrow) MpDemux();
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->stream.fp = open_utf8(path);
    if (d->stream.fp == nullptr) {
        delete d;
        return MP_ERR_IO;
    }

    char why[80] = {};
    // OPEN_DSD_NATIVE for the reason at the top of this file. OPEN_WVC is
    // deliberately *not* set: a `.wvc` correction file sits beside the `.wv`
    // and turns a hybrid file lossless, and finding it would mean this module
    // opening a second path the host never gave it. That is the host's business
    // and a thing to add when the host can express it.
    d->wpc = WavpackOpenFileInputEx64(&g_reader, &d->stream, nullptr, why,
                                      OPEN_DSD_NATIVE | OPEN_ALT_TYPES, 0);
    if (d->wpc == nullptr) {
        log_fmt(MP_LOG_DEBUG, "%s: libwavpack would not open it: %s", path, why);
        std::fclose(d->stream.fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    const int mode = WavpackGetMode(d->wpc);
    const int qmode = WavpackGetQualifyMode(d->wpc);
    d->is_dsd = (qmode & QMODE_DSD_AUDIO) != 0;
    d->is_float = (mode & MODE_FLOAT) != 0;
    const auto channels = static_cast<std::uint32_t>(WavpackGetNumChannels(d->wpc));
    const auto rate = static_cast<std::uint32_t>(WavpackGetSampleRate(d->wpc));
    const auto valid = static_cast<unsigned>(WavpackGetBitsPerSample(d->wpc));
    d->container = static_cast<unsigned>(WavpackGetBytesPerSample(d->wpc));
    const std::int64_t samples = WavpackGetNumSamples64(d->wpc);

    if (channels == 0 || channels > 8 || rate == 0 || samples < 0) {
        log_fmt(MP_LOG_DEBUG, "%s: %u channels at %u Hz is not a stream this reads", path,
                channels, rate);
        WavpackCloseFile(d->wpc);
        std::fclose(d->stream.fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    d->format.sample_rate = rate;
    d->format.channels = channels;
    d->format.channel_mask =
        channels > 2 ? static_cast<std::uint32_t>(WavpackGetChannelMask(d->wpc)) : 0u;
    d->format.encoding = MP_ENCODING_PCM;

    if (d->is_dsd) {
        // **A DSD WavPack file is a DSD file that was compressed**, and what
        // comes back with OPEN_DSD_NATIVE is one DSD byte per `int32`. So this
        // hands `codec_dsd` the same packets `demux_dsd` does, and the DoP
        // framing, the marker phase and the seek rounding are all that module's
        // -- written once.
        //
        // libwavpack reports the *byte* rate here, as FFmpeg does, so the DSD
        // rate is eight times it and a DoP frame is two bytes of it.
        d->codec = MP_CODEC_DSD;
        d->frame_bytes = channels; // one DSD byte per channel per frame
        const std::uint32_t dsd_rate = rate * 8u;
        const std::uint32_t dop_rate = rate / 2u;
        std::memcpy(d->config, &dsd_rate, 4);
        d->config[4] = static_cast<std::uint8_t>(channels);
        std::memcpy(d->config + 5, &d->format.channel_mask, 4);
        d->config_bytes = sizeof(d->config);
        // What the codec will produce, for the reason demux_dsd states at
        // length: everything the host counts is in the codec's frames.
        d->format.sample_rate = dop_rate;
        d->format.sample_type = MP_SAMPLE_NONE;
        d->format.encoding = MP_ENCODING_DOP;
        d->total_frames = static_cast<std::uint64_t>(samples) / 2u;
    } else if (d->is_float) {
        // Float WavPack is 32-bit IEEE in the `int32` words, bit for bit.
        d->format.sample_type = MP_SAMPLE_F32;
        d->frame_bytes = 4 * channels;
        d->total_frames = static_cast<std::uint64_t>(samples);
    } else {
        d->format.sample_type = sample_type_for(d->container, valid);
        d->format.valid_bits = valid;
        if (d->format.sample_type == MP_SAMPLE_NONE) {
            // Eight-bit is the one WavPack writes that nothing here takes: it
            // is unsigned in WAV and signed in WavPack, so reaching a sample
            // type would mean a conversion. `codec_pcm` refuses 8-bit for the
            // same reason and this refuses it in the same words.
            log_fmt(MP_LOG_DEBUG, "%s: %u bits in %u bytes is not a container this reads",
                    path, valid, d->container);
            WavpackCloseFile(d->wpc);
            std::fclose(d->stream.fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->frame_bytes = static_cast<std::uint32_t>(d->container) * channels;
        d->total_frames = static_cast<std::uint64_t>(samples);
    }

    log_fmt(MP_LOG_DEBUG, "WavPack: %s%s%s, %u Hz, %u channels, %u bits",
            (mode & MODE_LOSSLESS) != 0 ? "lossless" : "lossy",
            (mode & MODE_HYBRID) != 0 ? " hybrid" : "", d->is_dsd ? ", DSD" : "", rate,
            channels, valid);

    d->unpacked.resize(static_cast<std::size_t>(k_packet_frames) * channels);
    *out = d;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_stream_count(MpDemux* d, std::uint32_t* out_count) noexcept
{
    if (d == nullptr || out_count == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_count = 1;
    return MP_OK;
}

MpResult MP_CALL demux_stream_info(MpDemux* d, std::uint32_t index, MpStreamInfo* out) noexcept
{
    if (d == nullptr || out == nullptr || index != 0) {
        return MP_ERR_INVALID;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, size);
    out->size = size;
    out->index = 0;
    out->kind = MP_STREAM_AUDIO;
    out->codec = d->codec;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = d->config_bytes;
    out->format = d->format;
    // **Exact, and it is the container that makes it so.** WavPack states its
    // sample count in the first block's metadata rather than implying it from a
    // file size, and it is lossless, so there is no encoder delay to edit out:
    // no `skip_frames`, no `play_frames`, nothing to trim.
    out->total_frames = d->total_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_needed = d->config_bytes;
    if (out == nullptr || d->config_bytes == 0) {
        return MP_OK;
    }
    if (out_bytes < d->config_bytes) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(out, d->config, d->config_bytes);
    return MP_OK;
}

MpResult MP_CALL demux_select(MpDemux* d, std::uint32_t index) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    return index == 0 ? MP_OK : MP_ERR_INVALID;
}

MpResult MP_CALL demux_read_packet(MpDemux* d, void* dst, std::size_t dst_bytes,
                                   MpPacket* out) noexcept
try {
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, size);
    out->size = size;

    const std::size_t want = static_cast<std::size_t>(k_packet_frames) * d->frame_bytes;
    if (dst == nullptr || dst_bytes < want) {
        out->bytes = static_cast<std::uint32_t>(want);
        return MP_ERR_NO_MEMORY;
    }

    const std::uint32_t got =
        WavpackUnpackSamples(d->wpc, d->unpacked.data(), k_packet_frames);
    if (got == 0) {
        return MP_END;
    }

    // **libwavpack always unpacks into `int32`, right-justified**, whatever the
    // file's width is: a 16-bit sample arrives sign-extended in a word. Every
    // container this module reports is the low bytes of that word in
    // little-endian order, which is what the ABI means by S16, S24_PACKED and
    // S32 -- so the narrowing is a byte copy and not arithmetic, and there is
    // nothing here that could round.
    const std::size_t samples = static_cast<std::size_t>(got) * d->format.channels;
    const auto* in = d->unpacked.data();
    auto* o = static_cast<std::uint8_t*>(dst);
    if (d->is_dsd) {
        // One DSD byte per word.
        for (std::size_t i = 0; i < samples; ++i) {
            o[i] = static_cast<std::uint8_t>(in[i] & 0xFF);
        }
    } else {
        const unsigned bytes = d->is_float ? 4u : d->container;
        for (std::size_t i = 0; i < samples; ++i) {
            const auto v = static_cast<std::uint32_t>(in[i]);
            for (unsigned b = 0; b < bytes; ++b) {
                o[i * bytes + b] = static_cast<std::uint8_t>((v >> (8u * b)) & 0xFFu);
            }
        }
    }

    out->bytes = static_cast<std::uint32_t>(samples * (d->is_dsd ? 1u : (d->is_float ? 4u : d->container)));
    out->frame = d->position;
    // Every packet stands alone: WavPack's blocks each carry their own sample
    // index and libwavpack has been asked to start at one, so there is nothing
    // to warm up.
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    d->position += d->is_dsd ? got / 2u : got;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    // A DSD file counts in DoP frames outside this module and in DSD bytes
    // inside it, and the rounding down to an even frame is `demux_dsd`'s: the
    // DoP marker alternates, so landing on an odd frame gives every frame after
    // it the wrong one.
    const std::uint64_t target = d->is_dsd ? (frame & ~1ull) * 2ull : frame;
    if (WavpackSeekSample64(d->wpc, static_cast<std::int64_t>(target)) == 0) {
        return MP_ERR_IO;
    }
    d->position = d->is_dsd ? target / 2ull : target;
    return MP_OK;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->wpc != nullptr) {
        WavpackCloseFile(d->wpc);
    }
    if (d->stream.fp != nullptr) {
        std::fclose(d->stream.fp);
    }
    delete d;
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

const MpDemuxVtbl g_vtbl = {
    /* size          */ sizeof(MpDemuxVtbl),
    /* reserved      */ 0,
    /* probe         */ &demux_probe,
    /* open          */ &demux_open,
    /* stream_count  */ &demux_stream_count,
    /* stream_info   */ &demux_stream_info,
    /* stream_config */ &demux_stream_config,
    /* select        */ &demux_select,
    /* read_packet   */ &demux_read_packet,
    /* seek          */ &demux_seek,
    /* read_frames   */ nullptr, // it names its payload's codec; see the top
    /* close         */ &demux_close,
};

const MpCodec g_codecs[] = {MP_CODEC_PCM, MP_CODEC_DSD};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 110,
    /* id          */ "demux_wavpack",
    /* name        */ "WavPack (libwavpack)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
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

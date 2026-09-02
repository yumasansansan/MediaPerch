// SPDX-License-Identifier: GPL-3.0-or-later
//
// WAV, and the four other containers dr_wav reads, as a container and nothing
// else.
//
// **The codec is a memcpy, and saying so is the point.** `decode_native` was a
// container reader and a decoder in one object, and for uncompressed audio the
// decoder half does nothing at all -- it hands over the bytes dr_wav read. v2
// makes that visible rather than implied: this module states the format the file
// is in and produces packets of the file's own samples, and `codec_pcm` copies
// them. What was gained is not tidiness. PCM lives in MP4, in Matroska, in CAF
// and in AIFF, and now that a PCM codec exists as a module of its own, any
// demuxer that names MP_CODEC_PCM has a decoder for it.
//
// Everything about *what the format is* moved here from `decode_native`, because
// all of it was the container's statement and never the codec's: the width, the
// valid bits, whether the samples are float, and the fact that 8-bit WAV is
// unsigned. dr_wav hands back the file's own bytes -- DR_WAV_NO_CONVERSION_API
// is on -- so what this reports is what the file holds.

#define DR_WAV_NO_CONVERSION_API

#include <dr_wav.h>

#include <mediaperch/module.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

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

/// The smallest container that holds `bits`, in bytes, or 0 if none does.
std::uint32_t container_for(std::uint32_t bits) noexcept
{
    if (bits == 0 || bits > 32) {
        return 0;
    }
    if (bits <= 16) {
        return 2;
    }
    if (bits <= 24) {
        return 3;
    }
    return 4;
}

/// Mirrors `mp::canonical_for`. Kept in step by the ABI test, not by hope.
MpSampleType sample_type_for(std::uint32_t container, std::uint32_t valid) noexcept
{
    if (valid == 0 || valid > container * 8) {
        return MP_SAMPLE_NONE;
    }
    switch (container) {
    case 2: return MP_SAMPLE_S16;
    case 3: return MP_SAMPLE_S24_PACKED;
    case 4: return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    default: return MP_SAMPLE_NONE;
    }
}

// The ABI carries paths as UTF-8. On Windows that has to become UTF-16 before it
// reaches the file system, or half this machine's music is unopenable.
#if defined(_WIN32)
std::wstring widen(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == '\0') {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}
#endif

bool open_wav(drwav* wav, const char* path)
{
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    return !wide.empty() && drwav_init_file_w(wav, wide.c_str(), nullptr) != 0;
#else
    return drwav_init_file(wav, path, nullptr) != 0;
#endif
}

bool has_prefix(const std::uint8_t* head, std::size_t bytes, const char* magic,
                std::size_t offset) noexcept
{
    const std::size_t n = std::strlen(magic);
    if (bytes < offset + n) {
        return false;
    }
    return std::memcmp(head + offset, magic, n) == 0;
}

/// Frames per packet.
///
/// PCM has no frames of its own, so the size is this module's choice, and the
/// choice is about buffers rather than about the format: large enough that the
/// per-packet cost disappears, small enough that a packet buffer is not a
/// megabyte. At 24-bit 7.1 this is 96 KB.
constexpr std::uint64_t k_packet_frames = 4096;

} // namespace

struct MpDemux {
    drwav wav{};
    bool ready = false;
    MpFormat format{};
    std::uint32_t frame_bytes = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t position = 0;

    ~MpDemux()
    {
        if (ready) {
            drwav_uninit(&wav);
        }
    }
};

namespace {

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                             std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr) {
        return MP_OK;
    }

    // Every container dr_wav reads, not only the one everybody remembers. RIFX
    // is big-endian WAV, RF64 is the form that gets past four gigabytes, W64 is
    // Sony's, and FORM/AIFF is Apple's.
    static const char* const wav_like[] = {"RIFF", "RIFX", "RF64", "riff", "FORM"};
    for (const char* magic : wav_like) {
        if (!has_prefix(head, bytes, magic, 0)) {
            continue;
        }
        // RIFF and RIFX carry "WAVE" at 8; AIFF carries "AIFF" or "AIFC"; RF64
        // and W64 are unambiguous from the first four bytes alone.
        const bool riff_like = (magic[1] == 'I');
        if (riff_like && !has_prefix(head, bytes, "WAVE", 8)) {
            continue;
        }
        if (magic[0] == 'F' && !has_prefix(head, bytes, "AIFF", 8) &&
            !has_prefix(head, bytes, "AIFC", 8)) {
            continue;
        }
        *out_score = 100;
        return MP_OK;
    }
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
    if (!open_wav(&d->wav, path)) {
        log_fmt(MP_LOG_DEBUG, "dr_wav would not open %s", path);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->ready = true;

    const std::uint32_t bits = d->wav.bitsPerSample;
    const std::uint32_t valid =
        d->wav.fmt.validBitsPerSample != 0 ? d->wav.fmt.validBitsPerSample : bits;
    const bool is_float = d->wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT;
    if (d->wav.translatedFormatTag != DR_WAVE_FORMAT_PCM && !is_float) {
        // ADPCM, mu-law and friends. dr_wav can decode some of them, but only by
        // converting -- and this is a container reader now, so there is nothing
        // here that could convert even if it wanted to. They are a codec id and
        // a codec module nobody has written, which is the honest shape for them.
        log(MP_LOG_WARN, "unsupported WAV encoding");
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    d->format.sample_rate = d->wav.sampleRate;
    d->format.channels = d->wav.channels;
    d->format.channel_mask = d->wav.fmt.channelMask;
    d->format.encoding = MP_ENCODING_PCM;

    std::uint32_t container = 0;
    if (is_float) {
        // 64-bit float costs nothing to carry: dr_wav hands back the file's own
        // bytes, and the only thing that ever stopped it was having no type to
        // name it with.
        if (bits == 64) {
            container = 8;
            d->format.sample_type = MP_SAMPLE_F64;
        } else if (bits == 32) {
            container = 4;
            d->format.sample_type = MP_SAMPLE_F32;
        } else {
            log_fmt(MP_LOG_WARN, "%u-bit float is not a format IEEE defines", bits);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->format.valid_bits = 0;
    } else if (bits == 8) {
        // Eight-bit WAV is unsigned -- silence is 128 -- which is a different
        // number line from everything else here, so it gets its own type rather
        // than being biased into S16 on the way past.
        container = 1;
        d->format.sample_type = MP_SAMPLE_U8;
        d->format.valid_bits = 8;
    } else {
        container = container_for(bits);
        d->format.sample_type = sample_type_for(container, valid);
        if (container == 0 || d->format.sample_type == MP_SAMPLE_NONE) {
            log_fmt(MP_LOG_DEBUG, "no container for %u bits (%u valid)", bits, valid);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->format.valid_bits = valid;
    }

    d->frame_bytes = container * d->format.channels;
    if (d->frame_bytes == 0) {
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->total_frames = d->wav.totalPCMFrameCount;
    log_fmt(MP_LOG_DEBUG, "%s: %u Hz, %u ch, %u bits (%u valid), container %u", path,
            d->format.sample_rate, d->format.channels, bits, valid, container);

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
    out->codec = MP_CODEC_PCM;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = 0; // MpStreamInfo::format is the whole of PCM's configuration
    out->format = d->format;
    out->total_frames = d->total_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    (void)out;
    (void)out_bytes;
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_needed = 0;
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
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, size);
    out->size = size;

    const std::uint64_t want = k_packet_frames * d->frame_bytes;
    if (dst == nullptr || dst_bytes < want) {
        // Nothing is consumed by a read that could not deliver.
        out->bytes = static_cast<std::uint32_t>(want);
        return MP_ERR_NO_MEMORY;
    }

    const drwav_uint64 got = drwav_read_pcm_frames(&d->wav, k_packet_frames, dst);
    if (got == 0) {
        return MP_END;
    }
    out->bytes = static_cast<std::uint32_t>(got * d->frame_bytes);
    out->frame = d->position;
    // Every PCM sample stands alone, so every packet begins a sync point.
    // Nothing else in this tree can say that, and it is why seeking here is
    // exact rather than to the nearest frame boundary.
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    d->position += got;
    return MP_OK;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    if (drwav_seek_to_pcm_frame(&d->wav, frame) == 0) {
        return MP_ERR_IO;
    }
    d->position = frame;
    return MP_OK;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
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
    /* read_frames   */ nullptr, // it splits properly; there is nothing to decode
    /* close         */ &demux_close,
};

const MpCodec g_codecs[] = {MP_CODEC_PCM};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 100,
    /* id          */ "demux_wav",
    /* name        */ "WAV, RIFX, RF64, W64 and AIFF (dr_wav)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
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

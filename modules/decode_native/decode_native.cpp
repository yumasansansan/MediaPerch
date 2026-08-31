// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder that has no dependencies: WAV and FLAC, from two public-domain
// headers, so an install with nothing else on disk still plays music.
//
// The one thing this file is careful about is **not converting anything**. A
// decoder reports the format the file is in and hands over those bytes; the
// graph decides what to do about it. dr_wav already reads WAV natively. dr_flac
// only offers s16/s32/f32 output, and its s32 is left-justified -- it shifts by
// `32 - bitsPerSample` -- so a 16-bit FLAC comes out as 16 bits sitting at the
// top of four bytes. Reporting that as a 32-bit source would be a lie about the
// file and would make the negotiator offer the four-byte container first, so the
// samples are moved back down into the container the file actually uses. That
// move is the same byte shuffle `repack` does in the core, and it is duplicated
// here in fifteen lines rather than making modules link the core.

#define DR_WAV_NO_CONVERSION_API
#define DR_FLAC_NO_OGG

#include <dr_flac.h>
#include <dr_wav.h>

#include <mediaperch/module.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

/// The smallest container that holds `bits`, in bytes, or 0 if none does.
// Deliberately not called logf: <cmath> has one, dr_libs pulls <math.h> in, and
// the overload that wins is the one that takes a float.
void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log(level, buffer);
}

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
// reaches the file system, or half this machine's music is unopenable; elsewhere
// UTF-8 is already what open(2) wants.
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

drflac* open_flac(const char* path)
{
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    return wide.empty() ? nullptr : drflac_open_file_w(wide.c_str(), nullptr);
#else
    return drflac_open_file(path, nullptr);
#endif
}

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

} // namespace

// The opaque handle from module.h.
struct MpDecoder {
    bool is_flac = false;
    drwav wav{};
    drflac* flac = nullptr;

    MpFormat format{};
    std::uint32_t container = 0; ///< bytes per sample we hand out
    std::uint32_t frame_bytes = 0;

    /// Only for FLAC: dr_flac's left-justified s32, before it is moved down into
    /// the file's own container.
    std::vector<drflac_int32> scratch;

    ~MpDecoder()
    {
        if (is_flac) {
            if (flac != nullptr) {
                drflac_close(flac);
            }
        } else {
            drwav_uninit(&wav);
        }
    }
};

namespace {

MpResult MP_CALL decoder_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                               std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    (void)path;
    *out_score = 0;

    if (head != nullptr) {
        if (has_prefix(head, bytes, "fLaC", 0)) {
            *out_score = 100;
        } else if (has_prefix(head, bytes, "RIFF", 0) && has_prefix(head, bytes, "WAVE", 8)) {
            *out_score = 100;
        }
    }
    return MP_OK;
}

MpResult MP_CALL decoder_open(const char* path, MpDecoder** out) noexcept
try {
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    auto decoder = new MpDecoder{};

    std::uint32_t bits = 0;
    std::uint32_t valid = 0;
    bool is_float = false;

    if ((decoder->flac = open_flac(path)) != nullptr) {
        decoder->is_flac = true;
        decoder->format.sample_rate = decoder->flac->sampleRate;
        decoder->format.channels = decoder->flac->channels;
        decoder->format.channel_mask = 0; // FLAC's channel assignment is implicit
        bits = decoder->flac->bitsPerSample;
        valid = bits;
    } else if (open_wav(&decoder->wav, path)) {
        decoder->is_flac = false;
        decoder->format.sample_rate = decoder->wav.sampleRate;
        decoder->format.channels = decoder->wav.channels;
        decoder->format.channel_mask = decoder->wav.fmt.channelMask;
        bits = decoder->wav.bitsPerSample;
        valid = decoder->wav.fmt.validBitsPerSample != 0
                    ? decoder->wav.fmt.validBitsPerSample
                    : bits;
        is_float = decoder->wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT;
        if (decoder->wav.translatedFormatTag != DR_WAVE_FORMAT_PCM && !is_float) {
            // ADPCM, mu-law and friends. dr_wav can decode some of them, but only
            // by converting, and a decoder that converts is not this one.
            log(MP_LOG_WARN, "unsupported WAV encoding");
            delete decoder;
            return MP_ERR_UNSUPPORTED;
        }
    } else {
        log_fmt(MP_LOG_DEBUG, "neither dr_flac nor dr_wav would open %s", path);
        delete decoder;
        return MP_ERR_UNSUPPORTED;
    }

    if (is_float) {
        if (bits != 32) {
            delete decoder;
            return MP_ERR_UNSUPPORTED;
        }
        decoder->container = 4;
        decoder->format.sample_type = MP_SAMPLE_F32;
        decoder->format.valid_bits = 0;
    } else {
        // Eight-bit WAV is unsigned, which is a different number line and would
        // need a conversion to reach any of our types. Refusing is honest.
        if (bits < 12) {
            log(MP_LOG_WARN, "8-bit PCM is unsigned and would need converting");
            delete decoder;
            return MP_ERR_UNSUPPORTED;
        }
        decoder->container = container_for(bits);
        decoder->format.sample_type = sample_type_for(decoder->container, valid);
        if (decoder->container == 0 || decoder->format.sample_type == MP_SAMPLE_NONE) {
            log_fmt(MP_LOG_DEBUG, "no container for %u bits (%u valid)", bits, valid);
            delete decoder;
            return MP_ERR_UNSUPPORTED;
        }
        log_fmt(MP_LOG_DEBUG, "%s: %u Hz, %u ch, %u bits (%u valid), container %u", path,
             decoder->format.sample_rate, decoder->format.channels, bits, valid,
             decoder->container);
        decoder->format.valid_bits = valid;
    }

    decoder->format.encoding = MP_ENCODING_PCM;
    decoder->frame_bytes = decoder->container * decoder->format.channels;
    if (decoder->frame_bytes == 0) {
        delete decoder;
        return MP_ERR_UNSUPPORTED;
    }

    *out = decoder;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
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
    *out_frames = d->is_flac ? d->flac->totalPCMFrameCount : d->wav.totalPCMFrameCount;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
try {
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t frames_wanted = dst_bytes / d->frame_bytes;
    if (frames_wanted == 0) {
        return MP_OK;
    }

    if (!d->is_flac) {
        // dr_wav reads the file's own bytes. Nothing to do to them.
        const drwav_uint64 got = drwav_read_pcm_frames(&d->wav, frames_wanted, dst);
        *out_bytes = static_cast<std::size_t>(got) * d->frame_bytes;
        return got == 0 ? MP_END : MP_OK;
    }

    const std::size_t samples_wanted = frames_wanted * d->format.channels;
    if (d->scratch.size() < samples_wanted) {
        d->scratch.resize(samples_wanted);
    }

    const drflac_uint64 got =
        drflac_read_pcm_frames_s32(d->flac, frames_wanted, d->scratch.data());
    if (got == 0) {
        return MP_END;
    }

    // dr_flac left-justifies into 32 bits. Keep the top `container` bytes, which
    // is exactly the file's own sample, and drop the padding underneath.
    const std::size_t samples = static_cast<std::size_t>(got) * d->format.channels;
    const auto* in = reinterpret_cast<const std::uint8_t*>(d->scratch.data());
    auto* out = static_cast<std::uint8_t*>(dst);
    const std::uint32_t skip = 4 - d->container;

    if (d->container == 4) {
        std::memcpy(out, in, samples * 4);
    } else {
        for (std::size_t i = 0; i < samples; ++i) {
            std::memcpy(out + i * d->container, in + i * 4 + skip, d->container);
        }
    }

    *out_bytes = samples * d->container;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    const bool ok = d->is_flac ? drflac_seek_to_pcm_frame(d->flac, frame) != 0
                               : drwav_seek_to_pcm_frame(&d->wav, frame) != 0;
    return ok ? MP_OK : MP_ERR_IO;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
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
    /* priority    */ 100,
    /* id          */ "decode_native",
    /* name        */ "WAV and FLAC, no dependencies",
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

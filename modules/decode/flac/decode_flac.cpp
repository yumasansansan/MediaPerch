// SPDX-License-Identifier: GPL-3.0-or-later
//
// FLAC, through libFLAC: the reference decoder, from Xiph, as a submodule.
//
// There is a reason to prefer the reference implementation for a *lossless*
// codec that does not apply to a lossy one. For MP3 or AAC, "correct" is a
// tolerance; for FLAC it is an identity, and the reference decoder is what
// defines it. `dr_flac` is smaller and has no build system at all, which is why
// `decode_native` exists -- but it silently decodes nothing at all for a 32-bit
// FLAC, because its frame-header table still marks the bit-depth code that FLAC
// 1.4 assigned to 32 bits as reserved. A reimplementation can drift from the
// spec; the spec cannot drift from itself.
//
// libFLAC also carries something no other decoder here can offer: **the file
// contains an MD5 of its own unencoded audio**, written by the encoder, and the
// decoder checks the samples it produced against it. That is a bit-exactness
// proof from inside the file, and this module turns it on and reports it.

#include <mediaperch/module.h>

#include <FLAC/stream_decoder.h>

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
#include <cstdio>
#include <cstring>
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

/// The smallest container that holds `bits`, in bytes.
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

std::FILE* open_binary(const char* utf8_path)
{
#if defined(_WIN32)
    // The ABI carries UTF-8. libFLAC's init_file takes a narrow path, which on
    // Windows means the ANSI code page and half this machine's music unopenable,
    // so the file is opened here and handed over as a FILE*.
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, nullptr, 0);
    if (needed <= 1) {
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide.data(), needed);
    std::FILE* file = nullptr;
    return ::_wfopen_s(&file, wide.c_str(), L"rb") == 0 ? file : nullptr;
#else
    return std::fopen(utf8_path, "rb");
#endif
}

} // namespace

struct MpDecoder {
    FLAC__StreamDecoder* decoder = nullptr;

    MpFormat format{};
    std::uint32_t bits = 0;
    std::uint32_t container = 0;
    std::uint32_t frame_bytes = 0;
    /// How far left the sample has to move to sit at the top of its container.
    /// Zero for 16, 24 and 32 bits; FLAC also allows 4, 8, 12 and 20.
    std::uint32_t shift = 0;
    std::uint64_t total_frames = 0;

    std::vector<std::uint8_t> pending;
    std::size_t pending_offset = 0;

    bool have_streaminfo = false;
    bool ended = false;
    bool had_error = false;
    /// A seek invalidates the running MD5, so the check is only meaningful for a
    /// decode that ran from beginning to end.
    bool md5_still_meaningful = true;
    std::string path;
};

namespace {

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder*,
                                              const FLAC__Frame* frame,
                                              const FLAC__int32* const buffer[],
                                              void* client)
{
    auto* d = static_cast<MpDecoder*>(client);
    if (!d->have_streaminfo || d->frame_bytes == 0) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    const std::uint32_t frames = frame->header.blocksize;
    const std::uint32_t channels = d->format.channels;

    const std::size_t at = d->pending.size();
    d->pending.resize(at + static_cast<std::size_t>(frames) * d->frame_bytes);
    std::uint8_t* out = d->pending.data() + at;

    // libFLAC hands over the sample's own value, sign-extended into an int32 and
    // right-aligned. Our containers are left-justified, which for 16, 24 and 32
    // bits is the same thing and for 20 or 12 is not.
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto value = static_cast<std::uint32_t>(buffer[c][i]) << d->shift;
            for (std::uint32_t b = 0; b < d->container; ++b) {
                *out++ = static_cast<std::uint8_t>((value >> (b * 8)) & 0xFFu);
            }
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_callback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata,
                       void* client)
{
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }
    auto* d = static_cast<MpDecoder*>(client);
    const auto& info = metadata->data.stream_info;

    d->bits = info.bits_per_sample;
    d->container = container_for(d->bits);
    d->shift = d->container != 0 ? d->container * 8 - d->bits : 0;

    d->format.sample_rate = info.sample_rate;
    d->format.channels = info.channels;
    d->format.channel_mask = 0; // FLAC's channel assignment is implicit
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = d->bits;
    d->format.sample_type = sample_type_for(d->container, d->bits);

    d->frame_bytes = d->container * info.channels;
    d->total_frames = info.total_samples;
    d->have_streaminfo = d->container != 0 && d->format.sample_type != MP_SAMPLE_NONE;
}

void error_callback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status,
                    void* client)
{
    auto* d = static_cast<MpDecoder*>(client);
    d->had_error = true;
    log_fmt(MP_LOG_WARN, "libFLAC: %s in %s",
            FLAC__StreamDecoderErrorStatusString[status], d->path.c_str());
}

MpResult MP_CALL decoder_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                               std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    (void)path;
    *out_score = 0;
    if (head != nullptr && bytes >= 4 && std::memcmp(head, "fLaC", 4) == 0) {
        *out_score = 100;
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
    decoder->path = path;

    decoder->decoder = FLAC__stream_decoder_new();
    if (decoder->decoder == nullptr) {
        delete decoder;
        return MP_ERR_NO_MEMORY;
    }

    // The whole reason to prefer this decoder: the file carries an MD5 of its own
    // unencoded audio and libFLAC will check what it produced against it.
    FLAC__stream_decoder_set_md5_checking(decoder->decoder, true);

    std::FILE* file = open_binary(path);
    if (file == nullptr) {
        FLAC__stream_decoder_delete(decoder->decoder);
        delete decoder;
        return MP_ERR_IO;
    }

    // libFLAC closes this FILE* in FLAC__stream_decoder_finish(). Closing it here
    // as well is a double free that only shows up on some CRTs.
    const FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_FILE(
        decoder->decoder, file, &write_callback, &metadata_callback, &error_callback,
        decoder);
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        log_fmt(MP_LOG_DEBUG, "libFLAC would not open %s: %s", path,
                FLAC__StreamDecoderInitStatusString[status]);
        FLAC__stream_decoder_delete(decoder->decoder);
        std::fclose(file);
        delete decoder;
        return MP_ERR_UNSUPPORTED;
    }

    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder->decoder) ||
        !decoder->have_streaminfo) {
        FLAC__stream_decoder_finish(decoder->decoder);
        FLAC__stream_decoder_delete(decoder->decoder);
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
    *out_frames = d->total_frames;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
try {
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t want = (dst_bytes / d->frame_bytes) * d->frame_bytes;
    if (want == 0) {
        return MP_OK;
    }

    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t written = 0;

    while (written < want) {
        if (d->pending_offset >= d->pending.size()) {
            d->pending.clear();
            d->pending_offset = 0;
            if (d->ended) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(d->decoder)) {
                d->ended = true;
                break;
            }
            const FLAC__StreamDecoderState state =
                FLAC__stream_decoder_get_state(d->decoder);
            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                d->ended = true;
            }
            if (d->pending.empty()) {
                if (d->ended) {
                    break;
                }
                continue; // a metadata block, not audio
            }
        }

        const std::size_t available = d->pending.size() - d->pending_offset;
        const std::size_t take = available < want - written ? available : want - written;
        std::memcpy(out + written, d->pending.data() + d->pending_offset, take);
        d->pending_offset += take;
        written += take;
    }

    *out_bytes = written;
    return written == 0 ? MP_END : MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    d->pending.clear();
    d->pending_offset = 0;
    d->ended = false;

    // libFLAC stops accumulating the MD5 once the stream has been seeked, so the
    // check at close would be meaningless from here on. Saying so is better than
    // reporting a mismatch that only means "we skipped some".
    d->md5_still_meaningful = false;

    if (!FLAC__stream_decoder_seek_absolute(d->decoder, frame)) {
        FLAC__stream_decoder_flush(d->decoder);
        return MP_ERR_IO;
    }
    return MP_OK;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->decoder != nullptr) {
        // finish() returns false when the MD5 in the file does not match what was
        // decoded -- which, for a decode that ran to the end without seeking, is
        // the file telling us we got it wrong. There is no other decoder here
        // that can be told that by its own input.
        const bool ok = FLAC__stream_decoder_finish(d->decoder) != 0;
        if (!ok && d->md5_still_meaningful && !d->had_error) {
            log_fmt(MP_LOG_ERROR,
                    "MD5 mismatch: %s decoded to something other than what its own "
                    "STREAMINFO says it should",
                    d->path.c_str());
        }
        FLAC__stream_decoder_delete(d->decoder);
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
    /* priority    */ 120, // above decode_native: the reference beats a reimplementation
    /* id          */ "decode_flac",
    /* name        */ "FLAC (libFLAC, the reference decoder)",
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

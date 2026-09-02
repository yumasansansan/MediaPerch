// SPDX-License-Identifier: GPL-3.0-or-later
//
// MP3, through dr_mp3 -- which is already in the tree, because it lives in the
// same submodule as the dr_wav and dr_flac that `decode_native` uses.
//
// This module exists for one measured reason. Media Foundation does not
// implement gapless metadata: given the same MP3, it returns 2544 frames more
// than the file contains and starts the audio 1729 frames -- 36 milliseconds --
// late, because it emits the encoder's warm-up as audio instead of discarding
// it. FFmpeg reads the LAME/Xing tag and gets the length exactly right, and so
// does dr_mp3: it parses the tag, skips `delayInPCMFrames` on the way out, stops
// at the padding boundary, and subtracts the delay from the frame count.
//
// So the choice here was not "a better MP3 decoder" -- it was the only one
// available that both handles gapless and costs no new dependency.
//
// The output is float, and deliberately. MP3's synthesis filterbank produces
// real numbers; dr_mp3 will happily hand back int16 instead, and that would be a
// quantisation performed inside a decoder, which nothing here does. The build
// defines DR_MP3_FLOAT_OUTPUT so that float is what the decoder actually
// computes rather than something converted back up from int16.

#include <mediaperch/module.h>

#include <dr_mp3.h>

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

/// Whether four bytes are a plausible MPEG Layer III frame header.
///
/// Checking the sync word alone is not enough: 0xFF followed by three set bits
/// turns up inside all sorts of files, and a probe that claims one of those has
/// taken it away from the decoder that could have read it. The version and
/// layer fields are both reserved-value-checked here, which costs two
/// comparisons and removes most of the false positives.
bool is_layer3_header(const std::uint8_t* h) noexcept
{
    if (h[0] != 0xFFu || (h[1] & 0xE0u) != 0xE0u) {
        return false;
    }
    const unsigned version = (h[1] >> 3) & 0x3u; // 01 is reserved
    const unsigned layer = (h[1] >> 1) & 0x3u;   // 01 is Layer III
    const unsigned bitrate = (h[2] >> 4) & 0xFu; // 1111 is invalid
    const unsigned rate = (h[2] >> 2) & 0x3u;    // 11 is reserved
    return version != 1u && layer == 1u && bitrate != 0xFu && rate != 3u;
}

/// How long the Layer III frame with this header is, in bytes, or 0 when the
/// header does not describe a length -- free format, or a reserved combination.
///
/// This exists so that a candidate header can be checked against the next one.
/// One sync pattern turns up by chance in any few kilobytes of compressed audio;
/// two that are a frame apart and agree about version, layer and sample rate do
/// not.
std::size_t layer3_frame_length(const std::uint8_t* h) noexcept
{
    static const unsigned mpeg1_kbps[16] = {0,  32,  40,  48,  56,  64,  80,  96,
                                            112, 128, 160, 192, 224, 256, 320, 0};
    static const unsigned mpeg2_kbps[16] = {0,  8,  16, 24, 32, 40,  48,  56,
                                            64, 80, 96, 112, 128, 144, 160, 0};
    // Indexed by the version field: 0 is MPEG 2.5, 1 is reserved, 2 is MPEG 2,
    // 3 is MPEG 1.
    static const unsigned rates[4][3] = {{11025, 12000, 8000},
                                         {0, 0, 0},
                                         {22050, 24000, 16000},
                                         {44100, 48000, 32000}};
    const unsigned version = (h[1] >> 3) & 0x3u;
    const unsigned rate = rates[version][(h[2] >> 2) & 0x3u];
    const unsigned kbps = version == 3u ? mpeg1_kbps[(h[2] >> 4) & 0xFu]
                                        : mpeg2_kbps[(h[2] >> 4) & 0xFu];
    if (rate == 0 || kbps == 0) {
        return 0;
    }
    // 1152 samples a frame on MPEG 1, 576 on MPEG 2 and 2.5.
    const unsigned per_frame = version == 3u ? 144u : 72u;
    return per_frame * kbps * 1000u / rate + ((h[2] >> 1) & 0x1u);
}

/// Whether two headers describe the same stream. A chance match will agree
/// about the sync bits and disagree about everything else.
bool same_stream(const std::uint8_t* a, const std::uint8_t* b) noexcept
{
    return (a[1] & 0x1Eu) == (b[1] & 0x1Eu) &&          // version and layer
           ((a[2] >> 2) & 0x3u) == ((b[2] >> 2) & 0x3u); // sample rate
}

#if defined(_WIN32)
std::wstring widen(const char* utf8)
{
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), len);
    return wide;
}
#endif

} // namespace

struct MpDecoder {
    drmp3 mp3{};
    bool open = false;
    MpFormat format{};
    std::uint64_t total_frames = 0;
    std::string path;
};

namespace {

MpResult MP_CALL decoder_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                               std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || bytes < 10) {
        return MP_OK;
    }

    std::size_t at = 0;

    // An ID3v2 tag sits in front of the audio and carries its own length as
    // four seven-bit bytes. Skipping it lands on the first frame header, which
    // is what actually identifies the file -- an ID3 tag by itself identifies
    // nothing, since FLAC and WAV files carry them too.
    if (std::memcmp(head, "ID3", 3) == 0) {
        const std::size_t size = (static_cast<std::size_t>(head[6] & 0x7Fu) << 21) |
                                 (static_cast<std::size_t>(head[7] & 0x7Fu) << 14) |
                                 (static_cast<std::size_t>(head[8] & 0x7Fu) << 7) |
                                 static_cast<std::size_t>(head[9] & 0x7Fu);
        at = 10 + size;
        if (at + 4 > bytes) {
            // The tag is bigger than the window we were given, so the frame
            // header is out of reach. Claim it weakly: this is very probably an
            // MP3, and open() is where being wrong costs nothing now that the
            // host tries the next candidate.
            *out_score = 60;
            return MP_OK;
        }
    }

    // **One sync pattern is not evidence.** A few kilobytes of somebody else's
    // compressed audio contains one by chance, and this module used to claim
    // those at 100 -- measured with `mediaperch-probe claims`, which found it
    // taking AMR, DTS and FLV files at full strength. That was not merely an
    // ordering wart: on those three it was the *only* claimant, so a file
    // FFmpeg could have read was refused outright.
    //
    // So a claim now needs two headers a frame apart that agree about version,
    // layer and sample rate. At an ordinary bit rate a frame is a few hundred
    // bytes, so a real MP3 confirms itself several times over inside the window
    // a probe is given.
    //
    // A file may still open with a few bytes of junk before the first frame,
    // which is why this scans rather than looking only at `at`.
    const std::size_t limit = bytes - 4 < at + 8192 ? bytes - 4 : at + 8192;
    for (std::size_t i = at; i <= limit; ++i) {
        if (!is_layer3_header(head + i)) {
            continue;
        }
        const std::size_t length = layer3_frame_length(head + i);
        if (length == 0) {
            continue; // free format: no length to follow, so nothing to confirm
        }
        if (i + length + 4 <= bytes && is_layer3_header(head + i + length) &&
            same_stream(head + i, head + i + length)) {
            *out_score = 100;
            return MP_OK;
        }
    }

    // Nothing confirmed, so nothing claimed -- not even weakly. A single
    // unconfirmed header was worth 60 in an earlier version of this, and 60 is
    // still a claim: on an AMR, a DTS stream and an FLV this module was the
    // *only* claimant, so the file was refused outright instead of reaching the
    // fallback that could read it. The weak claim that is still worth making is
    // the one above, where an ID3 tag proves the audio is out of reach rather
    // than absent.
    return MP_OK;
}

MpResult MP_CALL decoder_open(const char* path, MpDecoder** out) noexcept
{
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    MpDecoder* d = new (std::nothrow) MpDecoder();
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->path = path;

#if defined(_WIN32)
    const std::wstring wide = widen(path);
    const drmp3_bool32 ok =
        wide.empty() ? DRMP3_FALSE : drmp3_init_file_w(&d->mp3, wide.c_str(), nullptr);
#else
    const drmp3_bool32 ok = drmp3_init_file(&d->mp3, path, nullptr);
#endif
    if (ok == DRMP3_FALSE) {
        log_fmt(MP_LOG_DEBUG, "dr_mp3 would not open %s", path);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->open = true;

    if (d->mp3.sampleRate == 0 || d->mp3.channels == 0 || d->mp3.channels > 2) {
        log_fmt(MP_LOG_WARN, "%s claims %u Hz and %u channels, which MPEG Layer III does not",
                path, d->mp3.sampleRate, d->mp3.channels);
        drmp3_uninit(&d->mp3);
        delete d;
        return MP_ERR_FORMAT;
    }

    d->format.sample_rate = d->mp3.sampleRate;
    d->format.channels = d->mp3.channels;
    d->format.channel_mask = 0; // MPEG Layer III is mono or stereo, like every other one here
    d->format.sample_type = MP_SAMPLE_F32;
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = 0;

    // This walks the file to count frames, and it is worth it: the count it
    // returns is the one the LAME tag describes, with the encoder delay
    // subtracted and the padding excluded. That number is the whole reason this
    // module exists.
    d->total_frames = drmp3_get_pcm_frame_count(&d->mp3);
    if (d->mp3.delayInPCMFrames != 0 || d->mp3.paddingInPCMFrames != 0) {
        log_fmt(MP_LOG_DEBUG, "%s: gapless tag says %u frames of delay and %u of padding", path,
                d->mp3.delayInPCMFrames, d->mp3.paddingInPCMFrames);
    } else {
        log_fmt(MP_LOG_DEBUG,
                "%s carries no gapless tag; the encoder's delay is in the audio and "
                "there is nothing here that can know how much",
                path);
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
    *out_frames = d->total_frames;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
{
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t stride = static_cast<std::size_t>(d->format.channels) * sizeof(float);
    if (stride == 0) {
        return MP_ERR_INTERNAL;
    }
    const std::uint64_t want = dst_bytes / stride;
    if (want == 0) {
        return MP_OK;
    }

    const drmp3_uint64 got =
        drmp3_read_pcm_frames_f32(&d->mp3, want, static_cast<float*>(dst));
    *out_bytes = static_cast<std::size_t>(got) * stride;
    return got == 0 ? MP_END : MP_OK;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    return drmp3_seek_to_pcm_frame(&d->mp3, frame) != DRMP3_FALSE ? MP_OK : MP_ERR_IO;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->open) {
        drmp3_uninit(&d->mp3);
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
    /* priority    */ 105, // above decode_mf, which does not implement gapless
    /* id          */ "decode_mp3",
    /* name        */ "MP3 (dr_mp3, with the gapless tag applied)",
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

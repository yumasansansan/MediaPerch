// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder that ships with the operating system: Media Foundation's source
// reader. It covers what `decode_native` does not -- MP4/AAC, HEVC, WMA -- and
// it costs nothing to install, because it is already there.
//
// The question this module has to answer honestly is whether it is bit-exact.
// A source reader will happily insert a converter to produce whatever media type
// it is asked for, and the conversion is invisible: the samples simply come back
// different. So this module:
//
//   - reads the *native* media type first, and asks for uncompressed PCM at
//     exactly that channel count, rate and bit depth;
//   - reads back what the reader actually agreed to, and reports that as the
//     format, rather than reporting what it asked for;
//   - warns when the two differ, because a difference means a converter is in
//     the chain and the output is not the file's own samples any more.
//
// Whether it is bit-exact for a given format is then a measurement rather than a
// claim -- `mediaperch-probe decode --decoder mf` prints the hash.

#include <mediaperch/module.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

const MpHost* g_host = nullptr;
bool g_started = false;

// The stream selectors are negative enumerators in a DWORD-shaped parameter, so
// every use needs the cast written out once rather than seven times.
constexpr DWORD first_audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD all_streams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD media_source = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

void log(MpLogLevel level, const char* message) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, message);
    }
}

// Deliberately not called logf: <cmath> has one and its float overload wins.
void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log(level, buffer);
}

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

bool has(const std::uint8_t* head, std::size_t bytes, const char* magic, std::size_t offset)
{
    const std::size_t n = std::strlen(magic);
    return bytes >= offset + n && std::memcmp(head + offset, magic, n) == 0;
}

} // namespace

struct MpDecoder {
    ComPtr<IMFSourceReader> reader;
    MpFormat format{};
    std::uint32_t frame_bytes = 0;
    /// What ReadSample handed over and `read` has not returned yet.
    std::vector<std::uint8_t> pending;
    std::size_t pending_offset = 0;
    bool ended = false;
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
    if (head == nullptr) {
        return MP_OK;
    }

    // Media Foundation opens far more than this, but a probe that guesses is a
    // probe that steals files from a decoder that would have done better. These
    // are the ones nothing else in the tree handles.
    if (has(head, bytes, "ftyp", 4)) {
        *out_score = 100; // MP4, M4A
    } else if (has(head, bytes, "ID3", 0) ||
               (bytes >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) {
        *out_score = 100; // MP3
    } else if (has(head, bytes, "\x30\x26\xB2\x75", 0)) {
        *out_score = 100; // ASF, WMA
    } else if (has(head, bytes, "RIFF", 0) && has(head, bytes, "WAVE", 8)) {
        *out_score = 40; // it can, but decode_native does it without a converter
    } else if (has(head, bytes, "fLaC", 0)) {
        *out_score = 40;
    }
    return MP_OK;
}

/// Whether a media subtype is ALAC.
///
/// Two spellings, and the SDK constant is the one that never turns up. mfapi.h
/// builds MFAudioFormat_ALAC out of the two-byte WAVE tag 0x6C61 over the
/// standard media-subtype base, giving {00006C61-0000-0010-8000-00AA00389B71}.
/// What IMFSourceReader actually reports for an ALAC track is
/// {616C6163-767A-494D-B478-F29D25DC9037}: the four-character code 'alac' over
/// the base Media Foundation uses for the codecs it gained in Windows 8.
///
/// Comparing against the constant alone matches nothing, silently -- which is
/// how this check failed the first time it was written, and is the same shape of
/// bug as the one it exists to catch. Both are accepted; the literal is the one
/// that fires.
bool is_alac(const GUID& subtype) noexcept
{
    static const GUID mf_runtime_alac = {
        0x616C6163, 0x767A, 0x494D, {0xB4, 0x78, 0xF2, 0x9D, 0x25, 0xDC, 0x90, 0x37}};
    return subtype == mf_runtime_alac || subtype == MFAudioFormat_ALAC;
}

MpResult MP_CALL decoder_open(const char* path, MpDecoder** out) noexcept
try {
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return MP_ERR_INVALID;
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = ::MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        return MP_ERR_UNSUPPORTED;
    }

    reader->SetStreamSelection(all_streams, FALSE);
    hr = reader->SetStreamSelection(first_audio, TRUE);
    if (FAILED(hr)) {
        return MP_ERR_UNSUPPORTED;
    }

    // What the file really is, before anything is asked of the reader.
    ComPtr<IMFMediaType> native;
    hr = reader->GetNativeMediaType(first_audio, 0, &native);
    if (FAILED(hr)) {
        return MP_ERR_UNSUPPORTED;
    }

    UINT32 channels = 0;
    UINT32 rate = 0;
    UINT32 native_bits = 0;
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &native_bits);
    if (channels == 0 || rate == 0) {
        return MP_ERR_UNSUPPORTED;
    }
    // Media Foundation's ALAC decoder returns Apple's own channel order and
    // then labels it with a WAVE channel mask, so the mask and the samples
    // disagree. ALAC's eight-channel order is C, Lc, Rc, L, R, Ls, Rs, LFE;
    // WAVE's for the same mask is FL, FR, FC, LFE, BL, BR, FLC, FRC. Measured
    // on a 384 kHz 7.1 file: every sample was present and **not one of the
    // eight channels came back in its own slot**, which is worse than a refusal
    // because nothing about it looks wrong.
    //
    // Stereo and mono cannot have a layout bug, so only multichannel is
    // declined -- and declining hands the file to decode_ffmpeg, which was
    // measured putting all eight channels back exactly where they started.
    GUID native_subtype{};
    const bool have_subtype = SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &native_subtype));
    if (have_subtype) {
        // Data1 of an MFAudioFormat_* GUID is the WAVE format tag, which is the
        // only readable part of it.
        log_fmt(MP_LOG_DEBUG, "%s: native subtype tag 0x%04lX, %u ch, %u Hz", path,
                static_cast<unsigned long>(native_subtype.Data1), channels, rate);
    }
    if (channels > 2 && have_subtype && is_alac(native_subtype)) {
        log_fmt(MP_LOG_WARN,
                "%s is %u-channel ALAC: Media Foundation returns those channels in "
                "Apple's order while labelling them with a WAVE mask, so this module "
                "declines it rather than put every channel in the wrong speaker",
                path, channels);
        return MP_ERR_UNSUPPORTED;
    }

    // A compressed stream reports no bit depth of its own, so there is nothing
    // to match and the question becomes what to ask for. Ask for 32.
    //
    // The obvious default is 16, and it is wrong: every lossy codec here decodes
    // to float internally, so 16 bits is a quantisation performed inside the
    // decoder, silently, on a signal that had more in it. Measured on an Opus
    // track in MP4, asking 16 gets 16 and asking 32 gets 32 -- the resolution
    // was there for the asking. The fall-back below still runs when a decoder
    // will not produce it.
    const UINT32 asked_bits = native_bits != 0 ? native_bits : 32;

    ComPtr<IMFMediaType> want;
    hr = ::MFCreateMediaType(&want);
    if (FAILED(hr)) {
        return MP_ERR_INTERNAL;
    }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, asked_bits);

    hr = reader->SetCurrentMediaType(first_audio, nullptr,
                                     want.Get());
    if (FAILED(hr)) {
        // Some decoders will not produce the native depth as PCM. Fall back to
        // 16-bit and let the read-back report what that cost.
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader->SetCurrentMediaType(first_audio, nullptr,
                                         want.Get());
        if (FAILED(hr)) {
            return MP_ERR_UNSUPPORTED;
        }
    }

    // What the reader agreed to, which is not necessarily what was asked for.
    ComPtr<IMFMediaType> agreed;
    hr = reader->GetCurrentMediaType(first_audio, &agreed);
    if (FAILED(hr)) {
        return MP_ERR_INTERNAL;
    }

    UINT32 got_channels = 0;
    UINT32 got_rate = 0;
    UINT32 got_bits = 0;
    UINT32 got_valid = 0;
    UINT32 got_mask = 0;
    agreed->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &got_channels);
    agreed->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &got_rate);
    agreed->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &got_bits);
    if (FAILED(agreed->GetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &got_valid))) {
        got_valid = got_bits;
    }
    if (FAILED(agreed->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &got_mask))) {
        got_mask = 0;
    }

    if (native_bits != 0 && got_bits != native_bits) {
        log_fmt(MP_LOG_WARN,
             "%s is %u-bit but Media Foundation will only give %u-bit; a converter is "
             "in the chain and the output is not the file's own samples",
             path, native_bits, got_bits);
    }
    if (got_rate != rate || got_channels != channels) {
        log_fmt(MP_LOG_WARN, "Media Foundation changed the stream from %u Hz/%u ch to %u Hz/%u ch",
             rate, channels, got_rate, got_channels);
    }

    auto decoder = new MpDecoder{};
    decoder->reader = reader;
    decoder->format.sample_rate = got_rate;
    decoder->format.channels = got_channels;
    decoder->format.channel_mask = got_mask;
    decoder->format.encoding = MP_ENCODING_PCM;
    decoder->format.valid_bits = got_valid;
    decoder->format.sample_type = sample_type_for(got_bits / 8, got_valid);
    decoder->frame_bytes = (got_bits / 8) * got_channels;

    if (decoder->format.sample_type == MP_SAMPLE_NONE || decoder->frame_bytes == 0) {
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
    *out_frames = 0;

    PROPVARIANT duration;
    ::PropVariantInit(&duration);
    if (SUCCEEDED(d->reader->GetPresentationAttribute(
            media_source, MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        // 100-nanosecond units.
        *out_frames = duration.uhVal.QuadPart * d->format.sample_rate / 10'000'000ULL;
    }
    ::PropVariantClear(&duration);
    return MP_OK;
}

/// Pulls one sample from the reader into `pending`. Returns false at the end.
bool refill(MpDecoder* d) noexcept
{
    if (d->ended) {
        return false;
    }

    for (;;) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr =
            d->reader->ReadSample(first_audio, 0, nullptr, &flags,
                                  &timestamp, &sample);
        if (FAILED(hr)) {
            d->ended = true;
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            d->ended = true;
            return false;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            // The stream changed shape underneath us. Nothing downstream can be
            // told about that, so stopping is the only honest thing.
            log(MP_LOG_WARN, "the media type changed mid-stream; stopping");
            d->ended = true;
            return false;
        }
        if (!sample) {
            continue; // a gap, not an end
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            d->ended = true;
            return false;
        }

        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) {
            d->ended = true;
            return false;
        }
        d->pending.assign(data, data + length);
        d->pending_offset = 0;
        buffer->Unlock();

        if (length != 0) {
            return true;
        }
    }
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
    const std::size_t want = frames_wanted * d->frame_bytes;

    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t written = 0;

    while (written < want) {
        if (d->pending_offset >= d->pending.size()) {
            if (!refill(d)) {
                break;
            }
        }
        const std::size_t available = d->pending.size() - d->pending_offset;
        const std::size_t take = available < want - written ? available : want - written;
        std::memcpy(out + written, d->pending.data() + d->pending_offset, take);
        d->pending_offset += take;
        written += take;
    }

    // Never hand back a partial frame: the graph counts frames, not bytes.
    written -= written % d->frame_bytes;
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
    PROPVARIANT position;
    ::PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart =
        static_cast<LONGLONG>(frame * 10'000'000ULL / d->format.sample_rate);
    const HRESULT hr = d->reader->SetCurrentPosition(GUID_NULL, position);
    ::PropVariantClear(&position);

    d->pending.clear();
    d->pending_offset = 0;
    d->ended = false;
    return SUCCEEDED(hr) ? MP_OK : MP_ERR_IO;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    delete d;
}

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        log(MP_LOG_ERROR, "MFStartup failed");
        return MP_ERR_INTERNAL;
    }
    g_started = true;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    if (g_started) {
        ::MFShutdown();
        g_started = false;
    }
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
    /* flags       */ MP_MODULE_NO_UNLOAD, // Media Foundation starts threads of its own
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DECODER,
    /* priority    */ 50, // below decode_native: that one needs no converter
    /* id          */ "decode_mf",
    /* name        */ "Media Foundation (the OS decoder)",
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

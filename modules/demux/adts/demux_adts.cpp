// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raw AAC in ADTS framing, as a container.
//
// **This is the module the v2 plan forgot.** The plan's table turned
// `decode_aac` into `codec_aac` and said its MP4 half went to `demux_mp4` --
// which is true, and left out that `decode_aac` had a *third* part: the ADTS
// framer. A raw `.aac` file is not an MP4 and not an MPEG audio stream, and
// without this it would have gone from a format with a first-class reader to
// one only FFmpeg could open. Writing it is what keeps the migration's rule
// that every step leaves the tree playing what it played before.
//
// ADTS is the framing for AAC that has nowhere to keep configuration, so it
// repeats it in front of every frame: the object type, the sample rate index and
// the channel configuration, seven bytes at a time. Those are the same three
// fields an MP4 keeps once in its `AudioSpecificConfig`, so this module
// assembles that config out of the first header and hands the codec the same
// two bytes it would have got from an MP4 -- which is the point of the split:
// `codec_aac` cannot tell which container it was called from, and should not be
// able to.

#include <mediaperch/module.h>

#include <cstdarg>
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

#if defined(_WIN32)
FILE* open_file(const char* path)
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
FILE* open_file(const char* path) { return std::fopen(path, "rb"); }
#endif

/// The eleven rates AAC's four-bit index can name, plus 96 and 88.2 above them.
std::uint32_t rate_for_index(unsigned index) noexcept
{
    static const std::uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                            22050, 16000, 12000, 11025, 8000,  7350};
    return index < 13 ? rates[index] : 0;
}

/// An ADTS header. The same configuration an MP4 keeps in `esds`, repeated in
/// front of every frame because a raw stream has nowhere else to put it.
struct Adts {
    unsigned object_type = 0;
    unsigned rate_index = 0;
    unsigned channel_config = 0;
    unsigned header_bytes = 0;
    unsigned frame_bytes = 0;
};

bool parse_adts(const std::uint8_t* h, std::size_t bytes, Adts& out) noexcept
{
    if (bytes < 7) {
        return false;
    }
    // Syncword, and the layer field: 0xFF followed by set bits is not rare, and
    // layer must be 00 for ADTS.
    if (h[0] != 0xFFu || (h[1] & 0xF0u) != 0xF0u || (h[1] & 0x06u) != 0) {
        return false;
    }
    const bool protection_absent = (h[1] & 1u) != 0;
    out.object_type = ((h[2] >> 6) & 0x3u) + 1u;
    out.rate_index = (h[2] >> 2) & 0xFu;
    out.channel_config = static_cast<unsigned>(((h[2] & 1u) << 2) | ((h[3] >> 6) & 0x3u));
    out.frame_bytes = (static_cast<unsigned>(h[3] & 0x3u) << 11) |
                      (static_cast<unsigned>(h[4]) << 3) |
                      (static_cast<unsigned>(h[5]) >> 5);
    const unsigned blocks = static_cast<unsigned>(h[6] & 0x3u) + 1u;
    out.header_bytes = protection_absent ? 7u : 9u;
    if (out.rate_index > 12 || out.frame_bytes <= out.header_bytes || blocks != 1) {
        return false;
    }
    return true;
}

/// Whether two headers describe the same stream. A chance sync agrees about the
/// sync bits and about very little else.
bool same_stream(const Adts& a, const Adts& b) noexcept
{
    return a.object_type == b.object_type && a.rate_index == b.rate_index &&
           a.channel_config == b.channel_config;
}

/// One AAC frame is always this many samples for the profiles here.
constexpr std::uint64_t k_frame_samples = 1024;

/// One remembered position in this many frames, as elsewhere.
constexpr std::uint64_t k_index_stride = 32;

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    std::uint64_t audio_at = 0;
    std::uint64_t file_bytes = 0;
    Adts first{};
    std::uint8_t config[2] = {0, 0};
    MpFormat format{};

    std::uint64_t next_index = 0;
    std::vector<std::uint64_t> index; ///< byte offset of frame i * k_index_stride
};

namespace {

/// Reads the next frame header at the current position, stepping over anything
/// that is not one. False at the end of the audio.
bool next_header(MpDemux* d, Adts& out, std::uint64_t& at)
{
    at = static_cast<std::uint64_t>(_ftelli64(d->fp));
    std::uint8_t h[9];
    if (std::fread(h, 1, sizeof(h), d->fp) < 7) {
        return false;
    }
    if (parse_adts(h, sizeof(h), out) && same_stream(out, d->first)) {
        return true;
    }
    constexpr std::uint64_t k_resync_limit = 65536;
    for (std::uint64_t moved = 1; moved <= k_resync_limit; ++moved) {
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved), SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(h, 1, sizeof(h), d->fp) < 7) {
            return false;
        }
        if (parse_adts(h, sizeof(h), out) && same_stream(out, d->first)) {
            at += moved;
            return true;
        }
    }
    return false;
}

/// The byte offset of frame `target`, walking and indexing as far as it must.
bool offset_of(MpDemux* d, std::uint64_t target, std::uint64_t& out_at)
{
    const auto bucket = static_cast<std::size_t>(target / k_index_stride);
    std::size_t have = d->index.size() - 1;
    std::uint64_t at = d->index[have];
    std::uint64_t frame = static_cast<std::uint64_t>(have) * k_index_stride;
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
        return false;
    }
    if (bucket < d->index.size()) {
        have = bucket;
        at = d->index[bucket];
        frame = static_cast<std::uint64_t>(bucket) * k_index_stride;
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
            return false;
        }
    }
    while (frame < target) {
        Adts adts;
        if (!next_header(d, adts, at)) {
            return false;
        }
        at += adts.frame_bytes;
        ++frame;
        if (frame % k_index_stride == 0) {
            const auto next_bucket = static_cast<std::size_t>(frame / k_index_stride);
            if (d->index.size() == next_bucket) {
                d->index.push_back(at);
            }
        }
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
            return false;
        }
    }
    out_at = at;
    return true;
}

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                             std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || bytes < 16) {
        return MP_OK;
    }

    std::size_t at = 0;
    if (std::memcmp(head, "ID3", 3) == 0 && bytes >= 10) {
        at = 10 + ((static_cast<std::size_t>(head[6] & 0x7Fu) << 21) |
                   (static_cast<std::size_t>(head[7] & 0x7Fu) << 14) |
                   (static_cast<std::size_t>(head[8] & 0x7Fu) << 7) |
                   static_cast<std::size_t>(head[9] & 0x7Fu));
        if (at + 16 > bytes) {
            // **Nothing, not a weak claim.** `demux_mpa` claims 60 here
            // because an MP3 with cover art is most MP3s and its frame header
            // is genuinely out of reach; an ADTS stream with a large ID3v2 tag
            // in front of it is rare enough that the same guess costs more than
            // it buys. Measured: with a speculative 60 this module outranked
            // `demux_mpa` on priority for every big-tagged MP3, opened it,
            // failed, and made the host fall through -- the right answer by the
            // wrong route, and the exact wart the probe audit removed from
            // `decode_mp3`.
            return MP_OK;
        }
    }

    // Two headers a frame apart that agree, for the reason `demux_mpa` needs
    // the same rule: one sync pattern turns up by chance in any few kilobytes of
    // somebody else's compressed audio.
    const std::size_t limit = bytes < at + 8192 ? bytes : at + 8192;
    for (std::size_t i = at; i + 7 <= limit; ++i) {
        Adts here;
        if (!parse_adts(head + i, limit - i, here)) {
            continue;
        }
        Adts next;
        const std::size_t after = i + here.frame_bytes;
        if (after + 7 <= bytes && parse_adts(head + after, bytes - after, next) &&
            same_stream(here, next)) {
            *out_score = 100;
            return MP_OK;
        }
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
    d->fp = open_file(path);
    if (d->fp == nullptr) {
        delete d;
        return MP_ERR_IO;
    }
    if (_fseeki64(d->fp, 0, SEEK_END) != 0) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_IO;
    }
    d->file_bytes = static_cast<std::uint64_t>(_ftelli64(d->fp));

    std::uint8_t id3[10];
    std::uint64_t at = 0;
    if (_fseeki64(d->fp, 0, SEEK_SET) == 0 && std::fread(id3, 1, 10, d->fp) == 10 &&
        std::memcmp(id3, "ID3", 3) == 0) {
        at = 10 + ((static_cast<std::uint64_t>(id3[6] & 0x7Fu) << 21) |
                   (static_cast<std::uint64_t>(id3[7] & 0x7Fu) << 14) |
                   (static_cast<std::uint64_t>(id3[8] & 0x7Fu) << 7) |
                   static_cast<std::uint64_t>(id3[9] & 0x7Fu));
    }

    // The first frame, confirmed by the one after it.
    bool found = false;
    for (std::uint64_t moved = 0; moved <= 262144 && !found; ++moved) {
        std::uint8_t h[9];
        Adts here;
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved), SEEK_SET) != 0 ||
            std::fread(h, 1, sizeof(h), d->fp) < 7 || !parse_adts(h, sizeof(h), here)) {
            continue;
        }
        std::uint8_t after[9];
        Adts next;
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved + here.frame_bytes),
                      SEEK_SET) == 0 &&
            std::fread(after, 1, sizeof(after), d->fp) >= 7 &&
            parse_adts(after, sizeof(after), next) && same_stream(here, next)) {
            at += moved;
            d->first = here;
            found = true;
        }
    }
    if (!found) {
        log_fmt(MP_LOG_DEBUG, "%s: no ADTS frame this demuxer can confirm", path);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    // **The AudioSpecificConfig, assembled out of the frame header.** Five bits
    // of object type, four of rate index, four of channel configuration and
    // three zeroes for the rest of the GASpecificConfig -- which is byte for
    // byte what an MP4 would have handed over, so `codec_aac` is handed the same
    // configuration from either container and cannot tell them apart.
    d->config[0] = static_cast<std::uint8_t>((d->first.object_type << 3) |
                                             (d->first.rate_index >> 1));
    d->config[1] = static_cast<std::uint8_t>(((d->first.rate_index & 1u) << 7) |
                                             (d->first.channel_config << 3));

    d->format.sample_rate = rate_for_index(d->first.rate_index);
    d->format.encoding = MP_ENCODING_PCM;
    // **The sample type is the codec's**, and so is the channel layout: channel
    // configuration 0 puts the layout in a program config element inside the
    // first frame, which is a thing only a decoder can read. Where the header
    // does state a count this reports it, which is every real file.
    d->format.channels = d->first.channel_config;
    d->format.sample_type = MP_SAMPLE_NONE;

    d->audio_at = at;
    d->index.push_back(at);
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_IO;
    }
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
    out->codec = MP_CODEC_AAC_LC;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = 2;
    out->format = d->format;
    // **No length and no edit, because a raw stream states neither.** An MP4
    // carries `elst` and a frame count; ADTS carries nothing but frames, so
    // zero here says "the container did not say" rather than "there is none".
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_needed = 2;
    if (out == nullptr) {
        return MP_OK;
    }
    if (out_bytes < 2) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(out, d->config, 2);
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

    Adts adts;
    std::uint64_t at = 0;
    if (!next_header(d, adts, at)) {
        return MP_END;
    }
    const std::uint32_t payload = adts.frame_bytes - adts.header_bytes;
    if (dst == nullptr || dst_bytes < payload) {
        out->bytes = payload;
        (void)_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET);
        return MP_ERR_NO_MEMORY;
    }

    // **The header is the container's and does not go to the codec.** What
    // `codec_aac` receives is a raw_data_block, exactly what an MP4 sample is,
    // which is what makes the two containers interchangeable to it.
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at + adts.header_bytes), SEEK_SET) != 0 ||
        std::fread(dst, 1, payload, d->fp) != payload) {
        return MP_END;
    }

    out->bytes = payload;
    out->frame = d->next_index * k_frame_samples;
    // AAC frames overlap by half a window, so no frame is a sync point on its
    // own; `seek` hands back the one before as pre-roll and the host drops it.
    out->flags = MP_PACKET_TIMED;

    ++d->next_index;
    if (d->next_index % k_index_stride == 0) {
        const auto bucket = static_cast<std::size_t>(d->next_index / k_index_stride);
        if (d->index.size() == bucket) {
            d->index.push_back(at + adts.frame_bytes);
        }
    }
    return MP_OK;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    const std::uint64_t wanted = frame / k_frame_samples;
    // One frame of pre-roll: AAC's MDCT windows overlap by half, so a decoder
    // started cold on the target frame is wrong for its first output.
    const std::uint64_t start = wanted > 0 ? wanted - 1 : 0;
    std::uint64_t at = 0;
    if (!offset_of(d, start, at)) {
        return MP_ERR_IO;
    }
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
        return MP_ERR_IO;
    }
    d->next_index = start;
    return MP_OK;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->fp != nullptr) {
        std::fclose(d->fp);
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
    /* read_frames   */ nullptr, // it splits properly; there is nothing to decode
    /* close         */ &demux_close,
};

const MpCodec g_codecs[] = {MP_CODEC_AAC_LC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 108,
    /* id          */ "demux_adts",
    /* name        */ "AAC in ADTS framing (the frame headers)",
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

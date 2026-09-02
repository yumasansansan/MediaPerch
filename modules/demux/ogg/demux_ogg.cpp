// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ogg, as a container and nothing else.
//
// **This is the split that pays for itself in formats rather than in tidiness.**
// `decode_ogg` drives `vorbisfile` and `opusfile`, which are container *and*
// codec in one object each -- so an OggFLAC or a Speex stream is "an Ogg this
// module cannot read", and falls to FFmpeg. Here it is an Ogg whose codec
// nobody has written yet, which is a different sentence and a smaller gap: the
// container is read, the stream is described, and the day somebody writes
// `codec_flac` the file plays.
//
// Ogg is also the container that shows why MP4 needs `open` and this does not:
// every logical stream announces itself in its first packet, on the first page,
// which is why `decode_ogg`'s probe could already tell Vorbis from Opus from
// four kilobytes. The information is in reach, so the probe uses it -- and this
// demuxer uses the same fact to name codecs without decoding anything.

#include <ogg/ogg.h>

#include <mediaperch/module.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    if (g_host == nullptr || g_host->log == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_host->log(g_host->ctx, level, line);
}

FILE* open_utf8(const char* path) noexcept
{
#if defined(_WIN32)
    const int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (len <= 0) {
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), len);
    FILE* fp = nullptr;
    return _wfopen_s(&fp, wide.c_str(), L"rb") == 0 ? fp : nullptr;
#else
    return std::fopen(path, "rb");
#endif
}

/// How many header packets a codec puts in front of its audio, and what it
/// calls itself in the first of them.
struct Mapping {
    const char* magic;
    std::size_t magic_bytes;
    std::size_t at;
    MpCodec codec;
    int headers;
};

const Mapping k_mappings[] = {
    {"OpusHead", 8, 0, MP_CODEC_OPUS, 2},
    {"\x01vorbis", 7, 0, MP_CODEC_VORBIS, 3},
    // Ogg FLAC's mapping header, and then the STREAMINFO block. Named here
    // because naming it is the whole point: no codec module reads it yet, and
    // "a container this reads carrying a codec nobody has" is worth saying.
    {"\x7f" "FLAC", 5, 0, MP_CODEC_FLAC, 2},
    {"Speex   ", 8, 0, MP_CODEC_SPEEX, 2},
};

MpCodec identify(const unsigned char* packet, long bytes, int& headers) noexcept
{
    for (const Mapping& m : k_mappings) {
        if (static_cast<std::size_t>(bytes) >= m.at + m.magic_bytes &&
            std::memcmp(packet + m.at, m.magic, m.magic_bytes) == 0) {
            headers = m.headers;
            return m.codec;
        }
    }
    headers = 0;
    return MP_CODEC_UNKNOWN;
}

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    ogg_sync_state sync{};
    ogg_stream_state stream{};
    bool stream_ready = false;

    int serial = 0;
    MpCodec codec = MP_CODEC_UNKNOWN;
    int header_packets = 0;
    std::vector<std::uint8_t> config;

    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t pre_skip = 0;

    bool ended = false;
    /// A packet libogg handed over that the caller's buffer could not hold.
    std::vector<std::uint8_t> pending;
    std::uint64_t pending_frame = 0;
    std::string path;
};

namespace {

/// Reads one more page into `d->stream`, or reports the end of the file.
///
/// Pages of other logical streams are dropped: this demuxer plays one stream,
/// and the pages of the others are the video and subtitle work that §9 will
/// need `select` for.
bool next_page(MpDemux* d) noexcept
{
    ogg_page page;
    for (;;) {
        const int got = ogg_sync_pageout(&d->sync, &page);
        if (got == 1) {
            if (ogg_page_serialno(&page) == d->serial) {
                ogg_stream_pagein(&d->stream, &page);
                return true;
            }
            continue;
        }
        if (got < 0) {
            continue; // a hole; libogg has resynchronised and said so
        }
        char* buffer = ogg_sync_buffer(&d->sync, 8192);
        if (buffer == nullptr) {
            return false;
        }
        const std::size_t read = std::fread(buffer, 1, 8192, d->fp);
        if (read == 0) {
            return false;
        }
        ogg_sync_wrote(&d->sync, static_cast<long>(read));
    }
}

/// The granule position of the last page in the file, which is where Ogg keeps
/// its length. Nothing else states one: an Ogg stream is a list of pages and
/// only the last one knows how long it turned out to be.
std::uint64_t final_granule(FILE* fp) noexcept
{
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    const std::int64_t size = _ftelli64(fp);
    // 64 KB is comfortably more than Ogg's 64 KB *maximum* page, so the last
    // page begins inside it unless the file ends in something that is not one.
    const std::int64_t window = size < 65536 ? size : 65536;
    if (_fseeki64(fp, size - window, SEEK_SET) != 0) {
        return 0;
    }
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
    if (std::fread(tail.data(), 1, tail.size(), fp) != tail.size()) {
        return 0;
    }
    std::uint64_t granule = 0;
    for (std::size_t i = 0; i + 14 <= tail.size(); ++i) {
        if (std::memcmp(tail.data() + i, "OggS", 4) != 0) {
            continue;
        }
        std::uint64_t value = 0;
        for (int b = 7; b >= 0; --b) {
            value = (value << 8) | tail[i + 6 + static_cast<std::size_t>(b)];
        }
        // -1 means "this page ends no packet", which the last page of a stream
        // never does.
        if (value != ~std::uint64_t{0}) {
            granule = value;
        }
    }
    return granule;
}

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head,
                             std::size_t bytes, std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // The container, and only the container. What is inside is named in the
    // first page and `open` reads it -- but a probe that scored on the codec
    // would be answering a question the resolution rules no longer ask.
    *out_score = (head != nullptr && bytes >= 4 && std::memcmp(head, "OggS", 4) == 0)
                     ? 100u
                     : 0u;
    return MP_OK;
}

MpResult MP_CALL demux_open(const char* path, MpDemux** out) noexcept
{
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    auto* d = new (std::nothrow) MpDemux();
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->path = path;
    d->fp = open_utf8(path);
    if (d->fp == nullptr) {
        delete d;
        return MP_ERR_IO;
    }
    ogg_sync_init(&d->sync);

    // The beginning-of-stream pages all come before any audio, so one pass over
    // them finds every logical stream and what each one is.
    ogg_page page;
    bool found = false;
    for (int guard = 0; guard < 4096 && !found; ++guard) {
        const int got = ogg_sync_pageout(&d->sync, &page);
        if (got != 1) {
            if (got < 0) {
                continue;
            }
            char* buffer = ogg_sync_buffer(&d->sync, 8192);
            const std::size_t read =
                buffer == nullptr ? 0 : std::fread(buffer, 1, 8192, d->fp);
            if (read == 0) {
                break;
            }
            ogg_sync_wrote(&d->sync, static_cast<long>(read));
            continue;
        }
        if (ogg_page_bos(&page) == 0) {
            break; // past the headers, and nothing here was audio we can name
        }

        ogg_stream_state candidate;
        ogg_stream_init(&candidate, ogg_page_serialno(&page));
        ogg_stream_pagein(&candidate, &page);
        ogg_packet packet;
        if (ogg_stream_packetout(&candidate, &packet) == 1) {
            int headers = 0;
            const MpCodec codec = identify(packet.packet, packet.bytes, headers);
            if (codec != MP_CODEC_UNKNOWN) {
                d->serial = ogg_page_serialno(&page);
                d->codec = codec;
                d->header_packets = headers;
                d->stream = candidate;
                d->stream_ready = true;
                // The identification packet is the first thing the config
                // needs, and it has already been read.
                if (codec == MP_CODEC_VORBIS) {
                    const auto n = static_cast<std::uint32_t>(packet.bytes);
                    for (int b = 0; b < 4; ++b) {
                        d->config.push_back(static_cast<std::uint8_t>((n >> (8 * b)) & 0xFFu));
                    }
                }
                d->config.insert(d->config.end(), packet.packet,
                                 packet.packet + packet.bytes);
                if (codec == MP_CODEC_OPUS && packet.bytes >= 19) {
                    d->channels = packet.packet[9];
                    d->pre_skip = static_cast<std::uint64_t>(packet.packet[10]) |
                                  (static_cast<std::uint64_t>(packet.packet[11]) << 8);
                    // **Opus always decodes at 48 kHz**, whatever the original
                    // rate was. That is the codec, not a resample, and the rate
                    // in the header is the input's rather than the output's.
                    d->rate = 48000;
                } else if (codec == MP_CODEC_VORBIS && packet.bytes >= 16) {
                    d->channels = packet.packet[11];
                    d->rate = static_cast<std::uint32_t>(packet.packet[12]) |
                              (static_cast<std::uint32_t>(packet.packet[13]) << 8) |
                              (static_cast<std::uint32_t>(packet.packet[14]) << 16) |
                              (static_cast<std::uint32_t>(packet.packet[15]) << 24);
                }
                found = true;
                continue;
            }
        }
        ogg_stream_clear(&candidate);
    }

    if (!found) {
        log_fmt(MP_LOG_DEBUG, "%s: no audio stream this demuxer can name", path);
        ogg_sync_clear(&d->sync);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    // The remaining header packets, into the config blob. A codec is handed the
    // container's bytes verbatim; assembling them is the container's work.
    int have = 1;
    for (int guard = 0; guard < 4096 && have < d->header_packets; ++guard) {
        ogg_packet packet;
        const int got = ogg_stream_packetout(&d->stream, &packet);
        if (got == 1) {
            if (d->codec == MP_CODEC_VORBIS) {
                const auto n = static_cast<std::uint32_t>(packet.bytes);
                for (int b = 0; b < 4; ++b) {
                    d->config.push_back(static_cast<std::uint8_t>((n >> (8 * b)) & 0xFFu));
                }
                d->config.insert(d->config.end(), packet.packet,
                                 packet.packet + packet.bytes);
            }
            // Opus's second header is OpusTags, which is metadata rather than
            // configuration: it is read past and not kept.
            ++have;
            continue;
        }
        if (!next_page(d)) {
            break;
        }
    }

    // **Where the file pointer is matters**, because the packet reader carries
    // on from it and the length is at the other end of the file. Looking for the
    // last page moves it; not putting it back produced a stream that described
    // itself perfectly and then decoded nothing at all.
    const std::int64_t resume = _ftelli64(d->fp);
    const std::uint64_t granule = final_granule(d->fp);
    (void)_fseeki64(d->fp, resume, SEEK_SET);
    // Opus counts its granule at 48 kHz from before the pre-skip, so the audio
    // is what is left after it. Vorbis counts finished samples.
    d->total_frames = granule > d->pre_skip ? granule - d->pre_skip : granule;

    // Back to where the audio starts. The header pages have been consumed and
    // the packet reader carries on from here.
    *out = d;
    return MP_OK;
}

MpResult MP_CALL demux_stream_count(MpDemux* d, std::uint32_t* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = 1;
    return MP_OK;
}

MpResult MP_CALL demux_stream_info(MpDemux* d, std::uint32_t index,
                                   MpStreamInfo* out) noexcept
{
    if (d == nullptr || out == nullptr || index != 0) {
        return MP_ERR_INVALID;
    }
    out->index = 0;
    out->kind = MP_STREAM_AUDIO;
    out->codec = d->codec;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = static_cast<std::uint32_t>(d->config.size());
    out->format.sample_rate = d->rate;
    out->format.channels = d->channels;
    // Every codec Ogg carries here decodes to float, which is what puts these
    // files on Path B. Said by the container because the container knows, and
    // corrected by the codec if it disagrees.
    out->format.sample_type = MP_SAMPLE_F32;
    out->format.encoding = MP_ENCODING_PCM;
    out->total_frames = d->total_frames;
    // **Opus's pre-skip is the gapless edit**, and it is in the container's
    // identification header exactly as MP4's is in `elst`. libopus does not
    // discard it -- nothing in the codec knows it should -- so it is stated
    // here and the host applies it, in the one place that already applies MP4's.
    out->skip_frames = d->pre_skip;
    out->play_frames = d->total_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto needed = static_cast<std::uint32_t>(d->config.size());
    *out_needed = needed;
    if (out == nullptr) {
        return MP_OK;
    }
    if (out_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }
    if (needed != 0) {
        std::memcpy(out, d->config.data(), needed);
    }
    return MP_OK;
}

MpResult MP_CALL demux_select(MpDemux* d, std::uint32_t index) noexcept
{
    return d != nullptr && index == 0 ? MP_OK : MP_ERR_INVALID;
}

MpResult MP_CALL demux_read_packet(MpDemux* d, void* dst, std::size_t dst_bytes,
                                   MpPacket* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    out->bytes = 0;
    if (!d->pending.empty()) {
        // The packet a previous call could not fit. It was never consumed by
        // anybody but this module, which is what makes that promise keepable.
        if (dst == nullptr || dst_bytes < d->pending.size()) {
            out->bytes = static_cast<std::uint32_t>(d->pending.size());
            return MP_ERR_NO_MEMORY;
        }
        std::memcpy(dst, d->pending.data(), d->pending.size());
        out->bytes = static_cast<std::uint32_t>(d->pending.size());
        out->flags = MP_PACKET_SYNC;
        out->frame = d->pending_frame;
        d->pending.clear();
        return MP_OK;
    }
    if (d->ended) {
        return MP_END;
    }
    for (int guard = 0; guard < 4096; ++guard) {
        ogg_packet packet;
        const int got = ogg_stream_packetout(&d->stream, &packet);
        if (got == 1) {
            if (dst == nullptr || dst_bytes < static_cast<std::size_t>(packet.bytes)) {
                // libogg has already handed this packet over, so "nothing is
                // consumed" has to be arranged rather than assumed: it is kept
                // here and returned on the next call.
                d->pending.assign(packet.packet, packet.packet + packet.bytes);
                d->pending_frame = packet.granulepos == -1
                                       ? 0
                                       : static_cast<std::uint64_t>(packet.granulepos);
                out->bytes = static_cast<std::uint32_t>(packet.bytes);
                return MP_ERR_NO_MEMORY;
            }
            std::memcpy(dst, packet.packet, static_cast<std::size_t>(packet.bytes));
            out->bytes = static_cast<std::uint32_t>(packet.bytes);
            out->flags = MP_PACKET_SYNC;
            out->frame = packet.granulepos == -1
                             ? 0
                             : static_cast<std::uint64_t>(packet.granulepos);
            return MP_OK;
        }
        if (got < 0) {
            continue; // a hole, which libogg has already stepped over
        }
        if (!next_page(d)) {
            d->ended = true;
            return MP_END;
        }
    }
    d->ended = true;
    return MP_END;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    // **Linear, and that is a choice rather than an oversight.** Ogg keeps its
    // position in the granule of each page, so seeking properly means bisecting
    // the file on page boundaries -- which is what `vorbisfile` does and what
    // this should do. Restarting and reading forward is correct, costs one pass
    // over the file, and is honest about being the simple version; the fast one
    // is worth writing when something asks for it.
    if (_fseeki64(d->fp, 0, SEEK_SET) != 0) {
        return MP_ERR_IO;
    }
    ogg_stream_reset(&d->stream);
    ogg_sync_reset(&d->sync);
    d->ended = false;
    d->pending.clear();

    // Past the headers again, and then forward to the page holding `frame`.
    int skipped = 0;
    for (int guard = 0; guard < 1000000; ++guard) {
        ogg_packet packet;
        const int got = ogg_stream_packetout(&d->stream, &packet);
        if (got == 1) {
            if (skipped < d->header_packets) {
                ++skipped;
                continue;
            }
            if (packet.granulepos != -1 &&
                static_cast<std::uint64_t>(packet.granulepos) > frame + d->pre_skip) {
                return MP_OK;
            }
            continue;
        }
        if (got < 0) {
            continue;
        }
        if (!next_page(d)) {
            d->ended = true;
            return MP_OK; // past the end, which is a legal place to seek to
        }
    }
    return MP_ERR_INTERNAL;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->stream_ready) {
        ogg_stream_clear(&d->stream);
    }
    ogg_sync_clear(&d->sync);
    if (d->fp != nullptr) {
        std::fclose(d->fp);
    }
    delete d;
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
    /* read_frames   */ nullptr,
    /* close         */ &demux_close,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

/// What it can name. FLAC and Speex are here because it *reads* those Oggs and
/// says what is in them -- whether a codec module exists for them is a separate
/// question with a separate answer, which is the improvement.
const MpCodec g_codecs[] = {MP_CODEC_OPUS, MP_CODEC_VORBIS, MP_CODEC_FLAC,
                            MP_CODEC_SPEEX};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 110,
    /* id          */ "demux_ogg",
    /* name        */ "Ogg (libogg, the reference container)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ 4,
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

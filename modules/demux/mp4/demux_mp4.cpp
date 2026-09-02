// SPDX-License-Identifier: GPL-3.0-or-later
//
// MP4, as a container and nothing else.
//
// **This is the module the v1 design could not have.** `decode_alac` and
// `decode_aac` each carried half of it -- the same half, from the same shared
// parser -- and each had to claim every MP4 at full strength and then decline
// the ones that were not theirs, because the box naming the codec is in `moov`
// and `moov` may be at the end of a file a probe sees four kilobytes of.
//
// A demuxer is not a probe. It has the whole file, so it reads `moov` wherever
// the muxer put it, says what the codec is, and hands over the configuration
// blob and the packets. Which decoder gets them is then a table lookup on a
// number the container stated, rather than two modules taking turns.
//
// It reads exactly one audio track, which is what `mp::mp4` parses. Video and
// subtitles are §9's, and the interface is shaped for them now so that adding
// them is not another migration.

#include "mp4.hpp"

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

std::uint32_t container_for(unsigned bits) noexcept
{
    if (bits <= 16) {
        return 2;
    }
    return bits <= 24 ? 3 : 4;
}

MpSampleType sample_type_for(std::uint32_t container, unsigned valid) noexcept
{
    switch (container) {
    case 2:
        return MP_SAMPLE_S16;
    case 3:
        return MP_SAMPLE_S24_PACKED;
    default:
        return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    }
}

/// `moov` is a top-level box and where it sits is the muxer's choice: FFmpeg
/// writes it after the audio, `refalac` before. Walking the top level by header
/// alone finds it either way and reads only the box it wants.
///
/// This walk used to be copied into two decoders. It is here once now, which is
/// the smallest of the reasons for the split and the easiest to see.
bool read_moov(FILE* fp, std::vector<std::uint8_t>& out) noexcept
{
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        return false;
    }
    const std::int64_t file_bytes = _ftelli64(fp);
    if (file_bytes < 8) {
        return false;
    }

    std::int64_t at = 0;
    // A file is a short list of top-level boxes. A thousand is already absurd
    // and bounds the walk against a file describing a loop of empty ones.
    for (int guard = 0; guard < 1000 && at + 8 <= file_bytes; ++guard) {
        if (_fseeki64(fp, at, SEEK_SET) != 0) {
            return false;
        }
        std::uint8_t header[16];
        if (std::fread(header, 1, 8, fp) != 8) {
            return false;
        }
        std::uint64_t size = (static_cast<std::uint32_t>(header[0]) << 24) |
                             (static_cast<std::uint32_t>(header[1]) << 16) |
                             (static_cast<std::uint32_t>(header[2]) << 8) |
                             static_cast<std::uint32_t>(header[3]);
        std::int64_t header_bytes = 8;
        if (size == 1) {
            if (std::fread(header + 8, 1, 8, fp) != 8) {
                return false;
            }
            size = 0;
            for (int i = 0; i < 8; ++i) {
                size = (size << 8) | header[8 + i];
            }
            header_bytes = 16;
        } else if (size == 0) {
            size = static_cast<std::uint64_t>(file_bytes - at);
        }
        if (size < static_cast<std::uint64_t>(header_bytes) ||
            at + static_cast<std::int64_t>(size) > file_bytes) {
            return false;
        }

        if (std::memcmp(header + 4, "moov", 4) == 0) {
            const std::uint64_t body = size - static_cast<std::uint64_t>(header_bytes);
            // A moov large enough to matter describes millions of packets, and
            // mp4.cpp bounds those separately. 256 MB is past any real file and
            // is still a bound.
            if (body == 0 || body > (256ull << 20)) {
                return false;
            }
            out.resize(static_cast<std::size_t>(body));
            if (_fseeki64(fp, at + header_bytes, SEEK_SET) != 0) {
                return false;
            }
            return std::fread(out.data(), 1, out.size(), fp) == out.size();
        }
        at += static_cast<std::int64_t>(size);
    }
    return false;
}

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    mp::mp4::AudioTrack track;
    MpCodec codec = MP_CODEC_UNKNOWN;
    MpFormat format{};
    std::size_t next_packet = 0;
    std::string path;
};

namespace {

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head,
                             std::size_t bytes, std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    // **A question about the container only.** What is inside is not a probe's
    // business, which is the entire difference between this module and the two
    // it replaces: they both had to claim every MP4 and then find out.
    if (head != nullptr && bytes >= 12 && std::memcmp(head + 4, "ftyp", 4) == 0) {
        *out_score = 100;
    }
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

    std::vector<std::uint8_t> moov;
    if (!read_moov(d->fp, moov)) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    const char* why = "";
    if (!mp::mp4::parse_moov(moov.data(), moov.size(), d->track, &why)) {
        log_fmt(MP_LOG_DEBUG, "%s: %s", path, why);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    // The container's own name for the codec, mapped onto ours. A demuxer that
    // passed the fourcc straight through would make two containers spelling one
    // codec differently into two codecs.
    switch (d->track.codec) {
    case mp::mp4::k_codec_alac:
        d->codec = MP_CODEC_ALAC;
        break;
    case mp::mp4::k_codec_mp4a:
        d->codec = MP_CODEC_AAC_LC;
        break;
    default:
        // Read perfectly, and carrying something nothing here names. That is a
        // sentence a host can act on, unlike "this decoder declines".
        d->codec = MP_CODEC_UNKNOWN;
        break;
    }

    // What the *container* states about the audio. ALAC's own configuration
    // states the depth and AAC's does not, which is why the codec's answer wins
    // where the two differ -- the codec is the one producing the samples.
    const std::uint32_t container = container_for(24);
    d->format.sample_rate = d->track.media_timescale;
    d->format.channels = 0;
    d->format.sample_type = sample_type_for(container, 24);
    d->format.encoding = MP_ENCODING_PCM;
    *out = d;
    return MP_OK;
}

MpResult MP_CALL demux_stream_count(MpDemux* d, std::uint32_t* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    // One, because `mp::mp4` finds one audio track. The interface counts
    // because §9's video and subtitles will need it to, and an interface that
    // gained counting later would be another migration.
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
    out->config_bytes = static_cast<std::uint32_t>(d->track.config.size());
    out->format = d->format;
    out->total_frames = d->track.total_frames;
    // **The gapless edit, which was always the container's.** `elst` says how
    // much of the front is the encoder's warm-up and how much of the rest is
    // the audio. v1 applied this inside each decoder; now it is stated once and
    // applied once.
    out->skip_frames = d->track.skip_frames;
    out->play_frames = d->track.play_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto needed = static_cast<std::uint32_t>(d->track.config.size());
    *out_needed = needed;
    if (out == nullptr) {
        return MP_OK; // asked what it would take, which the ABI allows
    }
    if (out_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }
    if (needed != 0) {
        std::memcpy(out, d->track.config.data(), needed);
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
    if (d->next_packet >= d->track.packets.size()) {
        return MP_END;
    }
    const mp::mp4::Packet& packet = d->track.packets[d->next_packet];
    if (dst == nullptr || dst_bytes < packet.size) {
        // **Nothing is consumed.** The host grows its buffer and asks again,
        // which is the only way a packet larger than somebody's guess is not
        // silently lost.
        out->bytes = packet.size;
        return MP_ERR_NO_MEMORY;
    }
    if (_fseeki64(d->fp, static_cast<std::int64_t>(packet.offset), SEEK_SET) != 0) {
        return MP_ERR_IO;
    }
    if (std::fread(dst, 1, packet.size, d->fp) != packet.size) {
        return MP_ERR_IO;
    }
    out->bytes = packet.size;
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    out->frame = static_cast<std::uint64_t>(d->next_packet) * d->track.frames_per_packet;
    ++d->next_packet;
    return MP_OK;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    if (d->track.frames_per_packet == 0) {
        return MP_ERR_UNSUPPORTED;
    }
    // To the packet containing the frame, which is the nearest point the codec
    // can be started from. What precedes the target inside that packet is the
    // host's to discard, and warming the codec is the host's as well -- written
    // once there rather than once per decoder.
    const std::uint64_t index = frame / d->track.frames_per_packet;
    d->next_packet = index < d->track.packets.size()
                         ? static_cast<std::size_t>(index)
                         : d->track.packets.size();
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
    /* read_frames   */ nullptr, // it splits properly, so it does not decode
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

/// What it can produce, as data. A container carries what it carries, so this
/// is what a report may say rather than what the file will hold.
const MpCodec g_codecs[] = {MP_CODEC_ALAC, MP_CODEC_AAC_LC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 100,
    /* id          */ "demux_mp4",
    /* name        */ "MP4 (the container, written here)",
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

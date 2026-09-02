// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native FLAC container: `fLaC`, its metadata blocks, and the frames.
//
// **FLAC is the case where "the reference implementation does both halves" is
// most tempting and least true.** libFLAC reads the container and decodes, so it
// would have been easy to make this a self-decoding pipeline and move on. What
// that would have cost is visible one directory away: `demux_ogg` already reads
// OggFLAC, names the codec, and has nowhere to send it. A FLAC *codec* -- one
// that takes a STREAMINFO and frames and knows nothing about files -- is what
// makes that stream playable, and a codec that exists needs a demuxer that
// produces frames for it. So the split is done properly on both sides and
// OggFLAC starts playing as a consequence rather than as a special case.
//
// The framing itself is in `shared/flacframe`, and the reason it is a real
// parser rather than a scan for sync bytes is written there: a FLAC frame does
// not carry its length, so its end has to be found, and the format's own CRC-16
// is what turns finding into confirming.
//
// This module has no dependency of any kind, which is worth having on its own:
// the container is read the same way whatever decodes the frames, so a second
// FLAC codec -- one on a different library, or one written here -- would drop in
// beside `codec_flac` without any of this being touched. That is exactly what
// having a container reader was for.

#include "flacframe.hpp"

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

/// How much of the file is held in the window at once.
///
/// A frame has to fit inside it, and STREAMINFO's `max_frame` says how large one
/// can be -- but an encoder is allowed to write zero there, so the window grows
/// on demand rather than being sized once from a number that may be missing.
constexpr std::size_t k_window = 1u << 16;

/// One remembered position in this many frames. The same trade as
/// `demux_mpeg`'s, for the same reason.
constexpr std::uint64_t k_index_stride = 32;

/// A position in the stream: which sample, and where its frame begins.
///
/// **The list of these is not evenly spaced and must not be treated as if it
/// were.** Some come from the file's own SEEKTABLE, whose points sit wherever
/// the encoder chose; the rest are recorded on the way past, one in
/// `k_index_stride`. Indexing it arithmetically -- entry *i* is frame *i* times
/// the stride -- is true of the second kind and false of the first, and mixing
/// the two was a bug: after a seek in a file with a seek table, the counter that
/// decides where the next point goes was wrong by however far the encoder's
/// points were from evenly spaced. Each point carries its own sample instead.

struct Point {
    std::uint64_t sample = 0;
    std::uint64_t at = 0; ///< byte offset in the file
};

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    std::uint64_t audio_at = 0;
    std::uint64_t file_bytes = 0;

    flacframe::StreamInfo info{};
    std::vector<std::uint8_t> config; ///< the STREAMINFO block, verbatim
    MpFormat format{};

    /// The window: `window` holds `file_bytes` from `window_at` onwards.
    std::vector<std::uint8_t> window;
    std::uint64_t window_at = 0;
    std::size_t window_have = 0;
    std::size_t window_used = 0;

    std::uint64_t position = 0; ///< the next packet's first sample

    /// Sorted by sample: the SEEKTABLE's points, then whatever was recorded
    /// while reading. Never empty -- the start of the audio is always in it.
    std::vector<Point> index;
    /// Frames read since the last point was recorded.
    std::uint64_t since_point = 0;
};

namespace {

/// Fills the window so that at least `least` bytes are available from
/// `window_at + window_used`, moving what is unread to the front.
bool fill(MpDemux* d, std::size_t least)
{
    const std::size_t left = d->window_have - d->window_used;
    if (left >= least) {
        return true;
    }
    if (d->window.size() < least) {
        d->window.resize(least > k_window ? least : k_window);
    }
    if (d->window_used != 0) {
        std::memmove(d->window.data(), d->window.data() + d->window_used, left);
        d->window_at += d->window_used;
        d->window_used = 0;
        d->window_have = left;
    }
    if (_fseeki64(d->fp, static_cast<std::int64_t>(d->window_at + d->window_have),
                  SEEK_SET) != 0) {
        return false;
    }
    const std::size_t room = d->window.size() - d->window_have;
    const std::size_t got =
        std::fread(d->window.data() + d->window_have, 1, room, d->fp);
    d->window_have += got;
    return d->window_have - d->window_used >= least || got != 0;
}

/// Puts the window at `at`, discarding whatever it held.
void rewind_to(MpDemux* d, std::uint64_t at)
{
    d->window_at = at;
    d->window_have = 0;
    d->window_used = 0;
}

/// The next frame in the window, without consuming it. Grows the window until a
/// whole frame fits or the file runs out.
const std::uint8_t* peek_frame(MpDemux* d, std::size_t& out_length)
{
    for (;;) {
        const std::uint64_t at = d->window_at + d->window_used;
        if (at >= d->file_bytes) {
            return nullptr;
        }
        // Ask for a window that could hold the largest frame the encoder said it
        // wrote, or a page if it said nothing.
        const std::size_t want = d->info.max_frame != 0 ? d->info.max_frame + 64 : k_window;
        (void)fill(d, want);
        const std::size_t have = d->window_have - d->window_used;
        if (have == 0) {
            return nullptr;
        }
        const std::uint8_t* p = d->window.data() + d->window_used;
        const bool at_end = at + have >= d->file_bytes;
        const std::size_t length = flacframe::frame_length(p, have, d->info, at_end);
        if (length != 0) {
            out_length = length;
            return p;
        }
        if (at_end) {
            return nullptr; // the tail is not a frame this stream could contain
        }
        // Not enough bytes for a whole frame. Grow and read again; if that
        // cannot happen either, this file is not one we can follow.
        const std::size_t bigger = d->window.size() * 2;
        if (bigger > (1u << 26)) {
            return nullptr;
        }
        d->window.resize(bigger);
        if (!fill(d, bigger - 1)) {
            return nullptr;
        }
    }
}

/// Records where a frame begins, one in `k_index_stride`.
///
/// Appended only when it is past everything already known, which is what keeps
/// the list sorted whatever the SEEKTABLE contained and however playback
/// jumped around.
void remember(MpDemux* d, std::uint64_t sample, std::uint64_t at)
{
    if (d->since_point < k_index_stride) {
        ++d->since_point;
        return;
    }
    d->since_point = 0;
    if (sample > d->index.back().sample) {
        d->index.push_back(Point{sample, at});
    }
}

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                             std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || bytes < 4) {
        return MP_OK;
    }
    // Four bytes of magic and nothing else is needed: unlike MP4, a native FLAC
    // stream says what it is in its first word and says what is inside it in the
    // next forty.
    if (std::memcmp(head, "fLaC", 4) == 0) {
        *out_score = 100;
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

    std::uint8_t magic[4];
    if (_fseeki64(d->fp, 0, SEEK_SET) != 0 || std::fread(magic, 1, 4, d->fp) != 4 ||
        std::memcmp(magic, "fLaC", 4) != 0) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    // The metadata blocks, of which only STREAMINFO and SEEKTABLE are this
    // module's business. Everything else -- the tags, the cover art, the padding
    // -- belongs to whatever reads metadata, and reading past it is the whole
    // interaction a demuxer has with it.
    std::uint64_t at = 4;
    bool have_info = false;
    for (int guard = 0; guard < 4096; ++guard) {
        std::uint8_t header[4];
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0 ||
            std::fread(header, 1, 4, d->fp) != 4) {
            break;
        }
        const bool last = (header[0] & 0x80u) != 0;
        const unsigned type = header[0] & 0x7Fu;
        const std::uint32_t length = (static_cast<std::uint32_t>(header[1]) << 16) |
                                     (static_cast<std::uint32_t>(header[2]) << 8) |
                                     static_cast<std::uint32_t>(header[3]);
        at += 4;
        if (type == 0) { // STREAMINFO
            std::vector<std::uint8_t> raw(length);
            if (length < 34 || std::fread(raw.data(), 1, length, d->fp) != length ||
                !flacframe::parse_streaminfo(raw.data(), raw.size(), d->info)) {
                break;
            }
            // The blob the ABI defines for MP_CODEC_FLAC is the block itself,
            // without the four bytes that said how long it was.
            d->config.assign(raw.begin(), raw.begin() + 34);
            have_info = true;
        } else if (type == 3) { // SEEKTABLE
            const std::uint32_t points = length / 18;
            std::vector<std::uint8_t> raw(length);
            if (std::fread(raw.data(), 1, length, d->fp) == length) {
                for (std::uint32_t i = 0; i < points; ++i) {
                    const std::uint8_t* p = raw.data() + static_cast<std::size_t>(i) * 18;
                    std::uint64_t sample = 0;
                    std::uint64_t offset = 0;
                    for (int b = 0; b < 8; ++b) {
                        sample = (sample << 8) | p[b];
                        offset = (offset << 8) | p[8 + b];
                    }
                    // A placeholder point says "unused" with a sample number of
                    // all ones, and following one would seek into the metadata.
                    if (sample != 0xFFFFFFFFFFFFFFFFull) {
                        d->index.push_back(Point{sample, offset});
                    }
                }
            }
        }
        at += length;
        if (last) {
            break;
        }
    }
    if (!have_info) {
        log_fmt(MP_LOG_DEBUG, "%s: no STREAMINFO this demuxer could read", path);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    d->audio_at = at;
    // The seek table's offsets are relative to the first frame, and the index
    // this module keeps is absolute, so they are made absolute once, here.
    for (Point& point : d->index) {
        point.at += d->audio_at;
    }
    if (d->index.empty() || d->index.front().sample != 0) {
        d->index.insert(d->index.begin(), Point{0, d->audio_at});
    }

    d->format.sample_rate = d->info.sample_rate;
    d->format.channels = d->info.channels;
    d->format.channel_mask = 0; // FLAC's channel assignment is implicit
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = d->info.bits;
    // **The sample type is the codec's to state.** The container knows the depth
    // and says so through `valid_bits`; what a decoder puts those bits into is
    // its own answer, and the two FLAC codecs here could reasonably differ.
    d->format.sample_type = MP_SAMPLE_NONE;

    rewind_to(d, d->audio_at);
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
    out->codec = MP_CODEC_FLAC;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = static_cast<std::uint32_t>(d->config.size());
    out->format = d->format;
    out->total_frames = d->info.total_samples;
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
    std::memcpy(out, d->config.data(), needed);
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

    std::size_t length = 0;
    const std::uint8_t* frame = peek_frame(d, length);
    if (frame == nullptr) {
        return MP_END;
    }
    if (dst == nullptr || dst_bytes < length) {
        // Nothing is consumed: the window still holds the frame.
        out->bytes = static_cast<std::uint32_t>(length);
        return MP_ERR_NO_MEMORY;
    }

    flacframe::FrameHeader header{};
    (void)flacframe::parse_header(frame, length, header);
    const std::uint64_t sample =
        header.variable ? header.number : header.number * d->info.min_block;

    remember(d, sample, d->window_at + d->window_used);
    std::memcpy(dst, frame, length);
    d->window_used += length;
    d->position = sample + header.block_size;

    out->bytes = static_cast<std::uint32_t>(length);
    out->frame = sample;
    // **Every FLAC frame stands alone.** There is no reservoir and no overlap:
    // the format was designed so that a decoder can start at any frame, which is
    // why seeking here needs no pre-roll at all.
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
try {
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    // The nearest remembered point at or before the target: a seek table point
    // the encoder wrote, or one this module recorded on the way past. The list
    // is sorted, so the last one that qualifies is the nearest.
    Point best = d->index.front();
    for (const Point& point : d->index) {
        if (point.sample <= frame && point.sample >= best.sample) {
            best = point;
        }
    }
    rewind_to(d, best.at);
    d->position = best.sample;
    d->since_point = 0;

    // Then forward, a frame at a time, until the one holding the target. The
    // header says where each frame starts, so this reads headers and skips
    // bodies rather than decoding anything.
    for (;;) {
        std::size_t length = 0;
        const std::uint8_t* p = peek_frame(d, length);
        if (p == nullptr) {
            return MP_OK; // past the end: the next read reports it
        }
        flacframe::FrameHeader header{};
        if (!flacframe::parse_header(p, length, header)) {
            return MP_ERR_IO;
        }
        const std::uint64_t sample =
            header.variable ? header.number : header.number * d->info.min_block;
        if (sample + header.block_size > frame) {
            d->position = sample;
            return MP_OK; // leave it unconsumed: this is the packet to hand back
        }
        remember(d, sample, d->window_at + d->window_used);
        d->window_used += length;
    }
} catch (...) {
    return MP_ERR_NO_MEMORY;
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

const MpCodec g_codecs[] = {MP_CODEC_FLAC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 120,
    /* id          */ "demux_flac",
    /* name        */ "FLAC (the container, written here)",
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

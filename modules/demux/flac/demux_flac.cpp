// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native FLAC container, on libFLAC.
//
// **This was written by hand once, and that was a mistake worth recording.** A
// FLAC frame does not carry its length, so a demuxer has to find the end of one
// by finding the beginning of the next -- and the first version of this module
// did that itself, scanning forward while running the format's CRC-16 and
// testing every position where it read zero. The reasoning written here was that
// the reference decoder does not expose frame boundaries.
//
// It does. `FLAC__stream_decoder_skip_single_frame` advances one frame without
// reconstructing its samples, and `FLAC__stream_decoder_get_decode_position`
// says where that left the stream. libFLAC's own header documents the pair for
// exactly this use, in these words: *"separating a FLAC stream into frames for
// editing or storing in a container"*. So the tree had a hand-written parser
// over a stranger's bytes standing in front of the reference implementation of
// the same format, doing a job that implementation offers.
//
// What replaced it is smaller and safer in the way that matters. The scan is
// gone, and with it a bug the fuzzer found in ninety seconds -- a frame header
// can parse in fewer bytes than the shortest frame it implies, and the CRC loop
// began by reading past the end of its buffer. libFLAC's decoder is fuzzed
// continuously on OSS-Fuzz (`external/flac/oss-fuzz/decoder.cc`), which no local
// campaign is going to match.
//
// **The split is unaffected, which is the point worth being clear about.**
// libFLAC is used here as a *container reader*: it is asked where frames begin
// and end, and never to decode one. The frames go to `codec_flac`, which is the
// same module `demux_ogg` hands an OggFLAC to. Two containers, one codec,
// neither aware of the other.
//
// What remains this module's own is the seek index, because that is about
// positions rather than about parsing: the file's SEEKTABLE where it has one,
// plus a point recorded every so often on the way past.

#include <mediaperch/module.h>

#include "pcm_format.hpp"

#include <FLAC/stream_decoder.h>

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

/// How many bytes a sample of `bits` occupies. Shared -- see
/// modules/shared/pcm_format, and the drift that put it there.
std::uint32_t container_for(std::uint32_t bits) noexcept
{
    return mp::pcm::container_for(bits);
}

/// The sample type for `valid` significant bits in a `container`-byte slot.
///
/// **Its own copy of this used to live here**, as it did in five other modules,
/// and the copies had drifted -- see modules/shared/pcm_format.
MpSampleType sample_type_for(std::uint32_t container, std::uint32_t valid) noexcept
{
    return mp::pcm::sample_type_for(container, valid);
}

/// One remembered position: which sample, and where its frame begins.
///
/// **The list is not evenly spaced and must not be indexed as if it were.**
/// Some points come from the file's own SEEKTABLE and sit wherever the encoder
/// chose; the rest are recorded on the way past, one in `k_index_stride`
/// frames. Each carries its own sample number for that reason.
struct Point {
    std::uint64_t sample = 0;
    std::uint64_t at = 0;
};

/// One remembered position in this many frames.
constexpr std::uint64_t k_index_stride = 32;

} // namespace

struct MpDemux {
    /// libFLAC's handle on the file, and ours. Two of them on purpose: reading
    /// a packet's bytes means going back to where its frame started, and doing
    /// that on the decoder's own handle would mean moving its file pointer
    /// behind its back and hoping to put it back exactly.
    FILE* fp = nullptr;
    FILE* bytes = nullptr;
    FLAC__StreamDecoder* dec = nullptr;
    std::uint64_t file_bytes = 0;

    std::vector<std::uint8_t> config; ///< the STREAMINFO block, verbatim
    MpFormat format{};
    std::uint64_t total_samples = 0;
    bool have_info = false;

    std::uint64_t audio_at = 0; ///< byte offset of the first frame
    std::uint64_t position = 0; ///< the next packet's first sample
    /// Sorted by sample; never empty after `open`.
    std::vector<Point> index;
    std::uint64_t since_point = 0;

    ~MpDemux()
    {
        if (dec != nullptr) {
            FLAC__stream_decoder_delete(dec);
        }
        if (fp != nullptr) {
            std::fclose(fp);
        }
        if (bytes != nullptr) {
            std::fclose(bytes);
        }
    }
};

namespace {

// --------------------------------------------------------------------------
// The file, as libFLAC sees it
// --------------------------------------------------------------------------

FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[],
                                      std::size_t* bytes, void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    if (*bytes == 0) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    const std::size_t got = std::fread(buffer, 1, *bytes, d->fp);
    *bytes = got;
    return got == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                    : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 at,
                                      void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    return _fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) == 0
               ? FLAC__STREAM_DECODER_SEEK_STATUS_OK
               : FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
}

FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder*, FLAC__uint64* at,
                                      void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    const std::int64_t here = _ftelli64(d->fp);
    if (here < 0) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *at = static_cast<FLAC__uint64>(here);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder*, FLAC__uint64* out,
                                          void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    *out = d->file_bytes;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool eof_cb(const FLAC__StreamDecoder*, void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    return std::feof(d->fp) != 0 ? 1 : 0;
}

/// Never called. Nothing here decodes: `skip_single_frame` is the only thing
/// that reads a frame, and it does not reconstruct one.
FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder*, const FLAC__Frame*,
                                        const FLAC__int32* const[], void*)
{
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* meta, void* client)
{
    auto* d = static_cast<MpDemux*>(client);
    if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& info = meta->data.stream_info;
        const std::uint32_t container = container_for(info.bits_per_sample);
        d->format.sample_rate = info.sample_rate;
        d->format.channels = info.channels;
        d->format.channel_mask = 0; // FLAC's channel assignment is implicit
        d->format.encoding = MP_ENCODING_PCM;
        d->format.valid_bits = info.bits_per_sample;
        // **The sample type is the codec's to state.** The container knows the
        // depth and says so through `valid_bits`; what a decoder puts those
        // bits into is its own answer.
        d->format.sample_type = MP_SAMPLE_NONE;
        d->total_samples = info.total_samples;
        d->have_info = container != 0 && info.sample_rate != 0 &&
                       sample_type_for(container, info.bits_per_sample) != MP_SAMPLE_NONE;

        // **The blob the ABI defines for MP_CODEC_FLAC is the STREAMINFO block
        // itself**, and libFLAC hands it over parsed rather than raw -- so it is
        // written back out. Thirty-four bytes whose layout the format fixes,
        // which is what lets the ABI define the blob and mean something precise.
        d->config.assign(34, 0);
        std::uint8_t* p = d->config.data();
        p[0] = static_cast<std::uint8_t>((info.min_blocksize >> 8) & 0xFFu);
        p[1] = static_cast<std::uint8_t>(info.min_blocksize & 0xFFu);
        p[2] = static_cast<std::uint8_t>((info.max_blocksize >> 8) & 0xFFu);
        p[3] = static_cast<std::uint8_t>(info.max_blocksize & 0xFFu);
        p[4] = static_cast<std::uint8_t>((info.min_framesize >> 16) & 0xFFu);
        p[5] = static_cast<std::uint8_t>((info.min_framesize >> 8) & 0xFFu);
        p[6] = static_cast<std::uint8_t>(info.min_framesize & 0xFFu);
        p[7] = static_cast<std::uint8_t>((info.max_framesize >> 16) & 0xFFu);
        p[8] = static_cast<std::uint8_t>((info.max_framesize >> 8) & 0xFFu);
        p[9] = static_cast<std::uint8_t>(info.max_framesize & 0xFFu);
        const std::uint32_t packed =
            (info.sample_rate << 12) | (((info.channels - 1u) & 0x7u) << 9) |
            (((info.bits_per_sample - 1u) & 0x1Fu) << 4) |
            static_cast<std::uint32_t>((info.total_samples >> 32) & 0xFu);
        p[10] = static_cast<std::uint8_t>((packed >> 24) & 0xFFu);
        p[11] = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
        p[12] = static_cast<std::uint8_t>((packed >> 8) & 0xFFu);
        p[13] = static_cast<std::uint8_t>(packed & 0xFFu);
        const auto low = static_cast<std::uint32_t>(info.total_samples & 0xFFFFFFFFu);
        p[14] = static_cast<std::uint8_t>((low >> 24) & 0xFFu);
        p[15] = static_cast<std::uint8_t>((low >> 16) & 0xFFu);
        p[16] = static_cast<std::uint8_t>((low >> 8) & 0xFFu);
        p[17] = static_cast<std::uint8_t>(low & 0xFFu);
        std::memcpy(p + 18, info.md5sum, 16);
        return;
    }
    if (meta->type == FLAC__METADATA_TYPE_SEEKTABLE) {
        const auto& table = meta->data.seek_table;
        for (unsigned i = 0; i < table.num_points; ++i) {
            const auto& point = table.points[i];
            // A placeholder point says "unused" with a sample number of all
            // ones, and following one would seek into the metadata.
            if (point.sample_number != 0xFFFFFFFFFFFFFFFFull) {
                d->index.push_back(Point{point.sample_number, point.stream_offset});
            }
        }
    }
}

void error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void*)
{
    log_fmt(MP_LOG_WARN, "libFLAC: %s", FLAC__StreamDecoderErrorStatusString[status]);
}

/// Where libFLAC has read up to, in bytes.
bool position_of(MpDemux* d, std::uint64_t& out) noexcept
{
    FLAC__uint64 at = 0;
    if (FLAC__stream_decoder_get_decode_position(d->dec, &at) == 0) {
        return false;
    }
    out = at;
    return true;
}

/// Puts libFLAC back at `at`, looking for a frame sync.
bool restart_at(MpDemux* d, std::uint64_t at) noexcept
{
    if (FLAC__stream_decoder_flush(d->dec) == 0) {
        return false;
    }
    return _fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) == 0;
}

/// Records where a frame begins, one in `k_index_stride`. Appended only when it
/// is past everything already known, which keeps the list sorted whatever the
/// SEEKTABLE held and however playback jumped around.
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

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

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
    // stream says what it is in its first word and what is inside it in the next
    // forty.
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
    d->bytes = open_file(path);
    if (d->fp == nullptr || d->bytes == nullptr) {
        delete d;
        return MP_ERR_IO;
    }
    if (_fseeki64(d->fp, 0, SEEK_END) != 0) {
        delete d;
        return MP_ERR_IO;
    }
    d->file_bytes = static_cast<std::uint64_t>(_ftelli64(d->fp));
    if (_fseeki64(d->fp, 0, SEEK_SET) != 0) {
        delete d;
        return MP_ERR_IO;
    }

    d->dec = FLAC__stream_decoder_new();
    if (d->dec == nullptr) {
        delete d;
        return MP_ERR_NO_MEMORY;
    }
    // Nothing here decodes, so there is nothing for the MD5 to check.
    FLAC__stream_decoder_set_md5_checking(d->dec, false);
    // STREAMINFO always arrives; the seek table has to be asked for, and it is
    // the one other block a container reader has any use for.
    FLAC__stream_decoder_set_metadata_respond(d->dec, FLAC__METADATA_TYPE_SEEKTABLE);

    if (FLAC__stream_decoder_init_stream(d->dec, &read_cb, &seek_cb, &tell_cb, &length_cb,
                                         &eof_cb, &write_cb, &metadata_cb, &error_cb,
                                         d) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        delete d;
        return MP_ERR_INTERNAL;
    }
    if (FLAC__stream_decoder_process_until_end_of_metadata(d->dec) == 0 || !d->have_info) {
        log_fmt(MP_LOG_DEBUG, "%s: no STREAMINFO this demuxer could use", path);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    if (!position_of(d, d->audio_at)) {
        delete d;
        return MP_ERR_IO;
    }

    // The seek table's offsets are relative to the first frame; the index this
    // module keeps is absolute, so they are made absolute once, here.
    for (Point& point : d->index) {
        point.at += d->audio_at;
    }
    if (d->index.empty() || d->index.front().sample != 0) {
        d->index.insert(d->index.begin(), Point{0, d->audio_at});
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
    out->codec = MP_CODEC_FLAC;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = static_cast<std::uint32_t>(d->config.size());
    out->format = d->format;
    out->total_frames = d->total_samples;
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

MpResult MP_CALL demux_select_streams(MpDemux* d, const std::uint32_t* indices,
                                      std::uint32_t count) noexcept
{
    if (d == nullptr || indices == nullptr || count == 0) {
        return MP_ERR_INVALID;
    }
    // **One stream in this container, so there is nothing to interleave.** A
    // set of two is a caller asking for something the file cannot hold rather
    // than this module declining to try, and MP_ERR_UNSUPPORTED is the honest
    // difference between the two.
    if (count > 1) {
        return MP_ERR_UNSUPPORTED;
    }
    return indices[0] == 0 ? MP_OK : MP_ERR_INVALID;
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

    std::uint64_t start = 0;
    if (!position_of(d, start) || start >= d->file_bytes) {
        return MP_END;
    }

    if (FLAC__stream_decoder_skip_single_frame(d->dec) == 0) {
        return MP_END;
    }
    const FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(d->dec);
    if (state == FLAC__STREAM_DECODER_END_OF_STREAM ||
        state == FLAC__STREAM_DECODER_ABORTED) {
        return MP_END;
    }

    std::uint64_t end = 0;
    if (!position_of(d, end) || end <= start) {
        return MP_END;
    }
    const std::uint64_t length = end - start;
    if (length > 0xFFFFFFFFull) {
        return MP_ERR_FORMAT;
    }

    if (dst == nullptr || dst_bytes < length) {
        // **Nothing is consumed by a read that could not deliver.** The frame
        // has already been skipped, so putting it back is arranged rather than
        // assumed: libFLAC is flushed and repositioned, and the next call reads
        // the same frame again.
        out->bytes = static_cast<std::uint32_t>(length);
        return restart_at(d, start) ? MP_ERR_NO_MEMORY : MP_ERR_IO;
    }

    // The frame's own bytes, on the handle libFLAC does not know about.
    if (_fseeki64(d->bytes, static_cast<std::int64_t>(start), SEEK_SET) != 0 ||
        std::fread(dst, 1, static_cast<std::size_t>(length), d->bytes) != length) {
        return MP_END;
    }

    const std::uint32_t blocksize = FLAC__stream_decoder_get_blocksize(d->dec);
    remember(d, d->position, start);
    out->bytes = static_cast<std::uint32_t>(length);
    out->frame = d->position;
    // **Every FLAC frame stands alone.** There is no reservoir and no overlap:
    // the format was designed so a decoder can start at any frame, which is why
    // seeking here needs no pre-roll at all.
    out->stream = 0; // the only one this container has
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    d->position += blocksize;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint32_t stream,
                            std::uint64_t frame) noexcept
try {
    if (d == nullptr || stream != 0) {
        return MP_ERR_INVALID;
    }
    // **The nearest remembered point at or before the target**, rather than
    // `FLAC__stream_decoder_seek_absolute`. That function decodes the frame it
    // lands on and leaves the stream past it, which is one block too far for a
    // demuxer: the packet the caller asked for would already be spent. The
    // index answers the same question and consumes nothing.
    Point best = d->index.front();
    for (const Point& point : d->index) {
        if (point.sample <= frame && point.sample >= best.sample) {
            best = point;
        }
    }
    if (!restart_at(d, best.at)) {
        return MP_ERR_IO;
    }
    d->position = best.sample;
    d->since_point = 0;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
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
    /* select_streams*/ &demux_select_streams,
    /* read_packet   */ &demux_read_packet,
    /* seek          */ &demux_seek,
    /* read_frames   */ nullptr, // it splits properly; there is nothing to decode
    /* close         */ &demux_close,
    /* stream_video_info */ nullptr, // FLAC is audio
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
    /* name        */ "FLAC (libFLAC, the reference framing)",
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

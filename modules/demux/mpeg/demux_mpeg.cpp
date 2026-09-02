// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPEG audio -- layers I, II and III -- as a container.
//
// **The container here is the frame headers**, which is a strange sentence until
// you notice how much of what `decode_mp3` did was container work: skipping the
// ID3v2 tag, finding the first frame, reading the LAME/Xing tag for the encoder
// delay and the padding, computing the length, and locating the frame that holds
// a given sample. None of that is decoding. dr_mp3 did it because dr_mp3 was
// both halves; here it is done once, in front of a codec that is handed frames
// and gives back samples.
//
// Two things fall out of the split that are worth having.
//
// **Layers I and II stop being somebody else's problem.** `decode_mp3` claimed
// only Layer III -- its probe tested for it explicitly -- so an MP2 went to
// FFmpeg, and without FFmpeg to Media Foundation, which is where
// [formats.md](../../../docs/formats.md) says nothing good happens. The frame
// header says which layer it is and dr_mp3 decodes all three, so this reads all
// three and names the codec MP_CODEC_MP1, MP2 or MP3 accordingly.
//
// **Seeking is exact.** The demuxer knows where every frame starts, so it can
// hand back the frame containing a sample *and the two before it*, which is what
// MP3's bit reservoir and its synthesis window need; the host decodes those and
// throws them away. Under v1 that was dr_mp3's private business and the seam did
// not exist to be got right.
//
// The gapless numbers are dr_mp3's, deliberately and to the digit: delay + 529
// and padding - 529, because the 529 is the decoder's own latency and dropping
// it here would move every MP3 in the library by twelve milliseconds relative to
// what this tree has already measured.

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

#if defined(_WIN32)
std::wstring widen(const char* utf8)
{
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), len);
    return wide;
}
#endif

FILE* open_file(const char* path)
{
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return nullptr;
    }
    FILE* fp = nullptr;
    return _wfopen_s(&fp, wide.c_str(), L"rb") == 0 ? fp : nullptr;
#else
    return std::fopen(path, "rb");
#endif
}

// --------------------------------------------------------------------------
// The frame header, which is the whole of MPEG audio's framing
// --------------------------------------------------------------------------

/// The version field: 0 is MPEG 2.5, 1 is reserved, 2 is MPEG 2, 3 is MPEG 1.
unsigned version_of(const std::uint8_t* h) noexcept { return (h[1] >> 3) & 0x3u; }
/// The layer field: 0 is reserved, 1 is Layer III, 2 is Layer II, 3 is Layer I.
unsigned layer_of(const std::uint8_t* h) noexcept { return (h[1] >> 1) & 0x3u; }
bool has_crc(const std::uint8_t* h) noexcept { return (h[1] & 0x1u) == 0; }
unsigned mode_of(const std::uint8_t* h) noexcept { return (h[3] >> 6) & 0x3u; }

std::uint32_t rate_of(const std::uint8_t* h) noexcept
{
    static const std::uint32_t rates[4][4] = {{11025, 12000, 8000, 0},
                                              {0, 0, 0, 0},
                                              {22050, 24000, 16000, 0},
                                              {44100, 48000, 32000, 0}};
    return rates[version_of(h)][(h[2] >> 2) & 0x3u];
}

std::uint32_t kbps_of(const std::uint8_t* h) noexcept
{
    // [MPEG 1 or 2/2.5][layer index 0=I, 1=II, 2=III][bitrate index]
    static const std::uint32_t table[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    const unsigned layer = layer_of(h);
    if (layer == 0) {
        return 0;
    }
    const unsigned mpeg1 = version_of(h) == 3u ? 0u : 1u;
    return table[mpeg1][3u - layer][(h[2] >> 4) & 0xFu];
}

std::uint32_t samples_per_frame(const std::uint8_t* h) noexcept
{
    switch (layer_of(h)) {
    case 3: return 384;                                    // Layer I
    case 2: return 1152;                                   // Layer II
    case 1: return version_of(h) == 3u ? 1152u : 576u;     // Layer III
    default: return 0;
    }
}

/// Whether four bytes could begin a frame. Structure only -- this is one sync
/// pattern, and one sync pattern is not evidence.
bool plausible(const std::uint8_t* h) noexcept
{
    return h[0] == 0xFFu && (h[1] & 0xE0u) == 0xE0u && version_of(h) != 1u &&
           layer_of(h) != 0u && ((h[2] >> 4) & 0xFu) != 0xFu &&
           ((h[2] >> 2) & 0x3u) != 3u && kbps_of(h) != 0 && rate_of(h) != 0;
}

/// The frame's length in bytes, or 0 when the header does not describe one --
/// free format, or a reserved combination.
std::size_t frame_length(const std::uint8_t* h) noexcept
{
    const std::uint32_t rate = rate_of(h);
    const std::uint32_t kbps = kbps_of(h);
    if (rate == 0 || kbps == 0) {
        return 0;
    }
    const unsigned pad = (h[2] >> 1) & 0x1u;
    if (layer_of(h) == 3u) {
        // Layer I counts in slots of four bytes, and its padding is one slot.
        return static_cast<std::size_t>((12u * kbps * 1000u / rate + pad) * 4u);
    }
    const unsigned per_frame = samples_per_frame(h) / 8u;
    return static_cast<std::size_t>(per_frame * kbps * 1000u / rate + pad);
}

/// Whether two headers describe the same stream. A chance match agrees about the
/// sync bits and disagrees about everything else.
bool same_stream(const std::uint8_t* a, const std::uint8_t* b) noexcept
{
    return (a[1] & 0x1Eu) == (b[1] & 0x1Eu) &&           // version and layer
           ((a[2] >> 2) & 0x3u) == ((b[2] >> 2) & 0x3u); // sample rate
}

MpCodec codec_of(const std::uint8_t* h) noexcept
{
    switch (layer_of(h)) {
    case 3: return MP_CODEC_MP1;
    case 2: return MP_CODEC_MP2;
    case 1: return MP_CODEC_MP3;
    default: return MP_CODEC_UNKNOWN;
    }
}

/// How many frame offsets are remembered: one in this many.
///
/// Every frame would be eight bytes each and eleven megabytes for a long
/// audiobook; one in thirty-two is a third of a megabyte for the same file and
/// costs a seek at most thirty-one frame headers of walking, which is a read of
/// a few kilobytes.
constexpr std::uint64_t k_index_stride = 32;

/// Frames handed back before the one the caller asked for, after a seek.
///
/// **MP3's bit reservoir reaches backwards.** A frame may store part of its
/// audio in up to 511 bytes of the frames before it, and the synthesis
/// filterbank carries half a window across the boundary as well, so a decoder
/// started cold on the target frame is audibly wrong for the first one. Two
/// frames is what the format's own limit asks for; the host decodes them and
/// discards them, which is what MP_PACKET_TIMED is for.
constexpr std::uint64_t k_preroll_frames = 2;

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    std::uint64_t audio_at = 0;  ///< byte offset of the first audio frame
    std::uint64_t next_index = 0;
    std::uint8_t header[4] = {0, 0, 0, 0};

    MpCodec codec = MP_CODEC_UNKNOWN;
    MpFormat format{};
    std::uint32_t spf = 0; ///< PCM frames per MPEG frame
    std::uint64_t total_frames = 0;
    std::uint64_t skip_frames = 0;
    std::uint64_t play_frames = 0;

    /// Byte offset of MPEG frame `i * k_index_stride`, built as the file is
    /// walked. `index[0]` is `audio_at` and is there from `open`.
    std::vector<std::uint64_t> index;
};

namespace {

/// Reads a header at the current position, resynchronising past anything that is
/// not one. False at the end of the audio.
bool next_header(MpDemux* d, std::uint8_t out[4], std::uint64_t& at)
{
    at = static_cast<std::uint64_t>(_ftelli64(d->fp));
    if (std::fread(out, 1, 4, d->fp) != 4) {
        return false;
    }
    if (plausible(out) && same_stream(out, d->header)) {
        return true;
    }
    // Junk between frames: an ID3v1 tag at the end, an APE tag, a byte lost in
    // transfer. Scanning is bounded because a file that needs more than this is
    // not an MPEG stream with a blemish, it is something else.
    constexpr std::uint64_t k_resync_limit = 65536;
    for (std::uint64_t moved = 1; moved <= k_resync_limit; ++moved) {
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved), SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(out, 1, 4, d->fp) != 4) {
            return false;
        }
        if (plausible(out) && same_stream(out, d->header)) {
            at += moved;
            return true;
        }
    }
    return false;
}

/// Walks forward from `from_index` at `from_at`, filling in the index, until
/// frame `target` is reached. Returns its byte offset, or false at the end.
bool walk_to(MpDemux* d, std::uint64_t from_index, std::uint64_t from_at,
             std::uint64_t target, std::uint64_t& out_at)
{
    if (_fseeki64(d->fp, static_cast<std::int64_t>(from_at), SEEK_SET) != 0) {
        return false;
    }
    std::uint64_t index = from_index;
    std::uint64_t at = from_at;
    while (index < target) {
        std::uint8_t h[4];
        if (!next_header(d, h, at)) {
            return false;
        }
        const std::size_t length = frame_length(h);
        if (length == 0) {
            return false;
        }
        at += length;
        ++index;
        if (index % k_index_stride == 0) {
            const auto bucket = static_cast<std::size_t>(index / k_index_stride);
            if (d->index.size() == bucket) {
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

/// The byte offset of MPEG frame `target`, extending the index if it has not
/// been walked that far yet.
bool offset_of(MpDemux* d, std::uint64_t target, std::uint64_t& out_at)
{
    const auto bucket = static_cast<std::size_t>(target / k_index_stride);
    while (d->index.size() <= bucket) {
        const std::size_t have = d->index.size() - 1; // the last one known
        const std::size_t before = d->index.size();
        std::uint64_t unused = 0;
        // Walking as far as the next boundary appends exactly one entry, unless
        // the audio ends first -- in which case the frame asked for is not
        // there and saying so is better than landing somewhere near it.
        if (!walk_to(d, static_cast<std::uint64_t>(have) * k_index_stride, d->index[have],
                     static_cast<std::uint64_t>(have + 1) * k_index_stride, unused)) {
            return false;
        }
        if (d->index.size() == before) {
            return false;
        }
    }
    return walk_to(d, static_cast<std::uint64_t>(bucket) * k_index_stride,
                   d->index[bucket], target, out_at);
}

/// The LAME/Xing tag, which is the only place an MP3 says how long it is and
/// what its encoder threw in front of the audio.
///
/// Every number here is dr_mp3's, because `decode_mp3` is what this replaces and
/// a length that differed by one frame would show up as a changed hash rather
/// than as a bug report.
bool read_gapless_tag(MpDemux* d, const std::uint8_t* frame, std::size_t bytes)
{
    const unsigned side = version_of(d->header) == 3u ? (mode_of(d->header) == 3u ? 17u : 32u)
                                                      : (mode_of(d->header) == 3u ? 9u : 17u);
    std::size_t at = 4u + (has_crc(d->header) ? 2u : 0u) + side;
    if (at + 8 > bytes) {
        return false;
    }
    const bool xing = std::memcmp(frame + at, "Xing", 4) == 0;
    const bool info = std::memcmp(frame + at, "Info", 4) == 0;
    if (!xing && !info) {
        return false;
    }
    const std::uint32_t flags = frame[at + 7];
    at += 8;

    if ((flags & 0x01u) != 0) { // FRAMES
        if (at + 4 > bytes) {
            return true;
        }
        const std::uint32_t frames = (static_cast<std::uint32_t>(frame[at]) << 24) |
                                     (static_cast<std::uint32_t>(frame[at + 1]) << 16) |
                                     (static_cast<std::uint32_t>(frame[at + 2]) << 8) |
                                     static_cast<std::uint32_t>(frame[at + 3]);
        d->total_frames = static_cast<std::uint64_t>(frames) * d->spf;
        at += 4;
    }
    if ((flags & 0x02u) != 0) { // BYTES
        at += 4;
    }
    if ((flags & 0x04u) != 0) { // TOC
        at += 100;
    }
    if ((flags & 0x08u) != 0) { // SCALE
        at += 4;
    }
    if (at >= bytes || frame[at] == 0 || at + 36 > bytes) {
        return true; // a tag, but no LAME extension: no delay and no padding
    }
    at += 21;
    const std::uint32_t delay =
        ((static_cast<std::uint32_t>(frame[at]) << 4) |
         (static_cast<std::uint32_t>(frame[at + 1]) >> 4)) +
        529u;
    const std::uint32_t stated = ((static_cast<std::uint32_t>(frame[at + 1]) & 0xFu) << 8) |
                                 static_cast<std::uint32_t>(frame[at + 2]);
    const std::uint32_t padding = stated > 529u ? stated - 529u : 0u;

    d->skip_frames = delay;
    if (d->total_frames > delay + padding) {
        d->play_frames = d->total_frames - delay - padding;
    }
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
    if (head == nullptr || bytes < 10) {
        return MP_OK;
    }

    std::size_t at = 0;
    // An ID3v2 tag sits in front of the audio and carries its own length as four
    // seven-bit bytes. Skipping it lands on the first frame header, which is
    // what actually identifies the file -- an ID3 tag identifies nothing, since
    // FLAC and WAV files carry them too.
    if (std::memcmp(head, "ID3", 3) == 0) {
        const std::size_t size = (static_cast<std::size_t>(head[6] & 0x7Fu) << 21) |
                                 (static_cast<std::size_t>(head[7] & 0x7Fu) << 14) |
                                 (static_cast<std::size_t>(head[8] & 0x7Fu) << 7) |
                                 static_cast<std::size_t>(head[9] & 0x7Fu);
        at = 10 + size;
        if (at + 4 > bytes) {
            // The tag is larger than the window a probe is given, so the frame
            // header is out of reach. Claim it weakly and let `open` decide,
            // which is where reading the whole file is allowed.
            *out_score = 60;
            return MP_OK;
        }
    }

    // **One sync pattern is not evidence**, and this module inherits the reason:
    // `decode_mp3` once claimed AMR, DTS and FLV at full strength by finding a
    // stray 0xFFE in somebody else's compressed audio, and on those three it was
    // the only claimant, so the file was refused outright rather than reaching
    // FFmpeg. A claim needs two headers a frame apart that agree.
    const std::size_t limit = bytes - 4 < at + 8192 ? bytes - 4 : at + 8192;
    for (std::size_t i = at; i <= limit; ++i) {
        if (!plausible(head + i)) {
            continue;
        }
        const std::size_t length = frame_length(head + i);
        if (length == 0) {
            continue;
        }
        if (i + length + 4 <= bytes && plausible(head + i + length) &&
            same_stream(head + i, head + i + length)) {
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

    // Past the ID3v2 tag, if there is one.
    std::uint8_t id3[10];
    std::uint64_t at = 0;
    if (std::fread(id3, 1, 10, d->fp) == 10 && std::memcmp(id3, "ID3", 3) == 0) {
        at = 10 + ((static_cast<std::uint64_t>(id3[6] & 0x7Fu) << 21) |
                   (static_cast<std::uint64_t>(id3[7] & 0x7Fu) << 14) |
                   (static_cast<std::uint64_t>(id3[8] & 0x7Fu) << 7) |
                   static_cast<std::uint64_t>(id3[9] & 0x7Fu));
    }

    // The first frame, confirmed by the one after it. `same_stream` needs a
    // reference header and this is where it comes from, so the first search is
    // done by hand rather than through `next_header`.
    std::uint8_t h[4];
    bool found = false;
    for (std::uint64_t moved = 0; moved <= 262144 && !found; ++moved) {
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved), SEEK_SET) != 0 ||
            std::fread(h, 1, 4, d->fp) != 4) {
            break;
        }
        if (!plausible(h)) {
            continue;
        }
        const std::size_t length = frame_length(h);
        if (length == 0) {
            continue;
        }
        std::uint8_t next[4];
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at + moved + length), SEEK_SET) != 0 ||
            std::fread(next, 1, 4, d->fp) != 4) {
            // The last frame of a very short file has nothing after it to agree
            // with, and a one-frame MPEG stream is still an MPEG stream.
            at += moved;
            found = true;
            break;
        }
        if (plausible(next) && same_stream(h, next)) {
            at += moved;
            found = true;
        }
    }
    if (!found) {
        log_fmt(MP_LOG_DEBUG, "%s: no MPEG audio frame this demuxer can confirm", path);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    std::memcpy(d->header, h, 4);
    d->codec = codec_of(h);
    d->spf = samples_per_frame(h);
    d->format.sample_rate = rate_of(h);
    d->format.channels = mode_of(h) == 3u ? 1u : 2u;
    d->format.encoding = MP_ENCODING_PCM;
    // **The sample type is not stated**, because it is not the container's to
    // state: the frame header says how many channels at what rate, and says
    // nothing about what a decoder will produce from them. `codec_mp3` answers
    // that, and the host takes the codec's word where the two overlap.
    d->format.sample_type = MP_SAMPLE_NONE;

    // The Xing/Info frame is a header wearing a frame's clothes: it decodes to
    // silence and its samples are not part of the audio, so it is read for what
    // it says and then stepped over.
    const std::size_t first_length = frame_length(h);
    std::vector<std::uint8_t> first(first_length);
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) == 0 &&
        std::fread(first.data(), 1, first_length, d->fp) == first_length) {
        if (read_gapless_tag(d, first.data(), first_length)) {
            at += first_length;
            log_fmt(MP_LOG_DEBUG,
                    "%s: gapless tag says %llu frames of delay and %llu to play", path,
                    static_cast<unsigned long long>(d->skip_frames),
                    static_cast<unsigned long long>(d->play_frames));
        }
    }

    d->audio_at = at;
    d->index.push_back(at);
    if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_IO;
    }

    if (d->total_frames == 0) {
        // No tag, so nothing said how long this is. Walking the headers is what
        // dr_mp3 does when asked, and doing it here buys the seek index at the
        // same time -- it reads the file once and decodes none of it.
        std::uint64_t end = 0;
        std::uint64_t index = 0;
        std::uint64_t cursor = at;
        for (; index < (1ull << 32); ++index) {
            std::uint8_t frame_header[4];
            if (!next_header(d, frame_header, cursor)) {
                break;
            }
            const std::size_t length = frame_length(frame_header);
            if (length == 0) {
                break;
            }
            cursor += length;
            if ((index + 1) % k_index_stride == 0) {
                const auto bucket = static_cast<std::size_t>((index + 1) / k_index_stride);
                if (d->index.size() == bucket) {
                    d->index.push_back(cursor);
                }
            }
            if (_fseeki64(d->fp, static_cast<std::int64_t>(cursor), SEEK_SET) != 0) {
                break;
            }
        }
        end = index;
        d->total_frames = end * d->spf;
        log_fmt(MP_LOG_DEBUG, "%s: no gapless tag; %llu frames counted by walking", path,
                static_cast<unsigned long long>(end));
        if (_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET) != 0) {
            std::fclose(d->fp);
            delete d;
            return MP_ERR_IO;
        }
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
    out->codec = d->codec;
    out->flags = MP_STREAM_DEFAULT;
    out->config_bytes = 4;
    out->format = d->format;
    out->total_frames = d->total_frames;
    out->skip_frames = d->skip_frames;
    out->play_frames = d->play_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index != 0 || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_needed = 4;
    if (out == nullptr) {
        return MP_OK;
    }
    if (out_bytes < 4) {
        return MP_ERR_NO_MEMORY;
    }
    // **MPEG audio's configuration is its first frame header**, and there is
    // nothing else it could be: the format carries no setup data at all. Four
    // bytes is enough for a codec to say what it will produce before it has
    // decoded anything, which is what `get_format` is asked for at open.
    std::memcpy(out, d->header, 4);
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

    std::uint8_t h[4];
    std::uint64_t at = 0;
    if (!next_header(d, h, at)) {
        return MP_END;
    }
    const std::size_t length = frame_length(h);
    if (length == 0) {
        return MP_END;
    }
    if (dst == nullptr || dst_bytes < length) {
        // Nothing is consumed by a read that could not deliver.
        out->bytes = static_cast<std::uint32_t>(length);
        (void)_fseeki64(d->fp, static_cast<std::int64_t>(at), SEEK_SET);
        return MP_ERR_NO_MEMORY;
    }

    auto* bytes = static_cast<std::uint8_t*>(dst);
    std::memcpy(bytes, h, 4);
    if (length > 4 && std::fread(bytes + 4, 1, length - 4, d->fp) != length - 4) {
        return MP_END; // a truncated last frame is the end of the audio
    }

    out->bytes = static_cast<std::uint32_t>(length);
    out->frame = d->next_index * d->spf;
    // **No MP_PACKET_SYNC**, and that is the honest answer: a Layer III frame
    // may keep part of its audio in the frames before it, so it cannot be
    // decoded alone. `seek` is what does something about that.
    out->flags = MP_PACKET_TIMED;

    ++d->next_index;
    if (d->next_index % k_index_stride == 0) {
        const auto bucket = static_cast<std::size_t>(d->next_index / k_index_stride);
        if (d->index.size() == bucket) {
            d->index.push_back(at + length);
        }
    }
    return MP_OK;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint64_t frame) noexcept
{
    if (d == nullptr || d->spf == 0) {
        return MP_ERR_INVALID;
    }
    const std::uint64_t wanted = frame / d->spf;
    const std::uint64_t start = wanted > k_preroll_frames ? wanted - k_preroll_frames : 0;
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

/// All three layers, because the frame header says which one it is and telling
/// the truth about that is free. Under v1 an MP2 went to FFmpeg.
const MpCodec g_codecs[] = {MP_CODEC_MP1, MP_CODEC_MP2, MP_CODEC_MP3};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 105,
    /* id          */ "demux_mpeg",
    /* name        */ "MPEG audio, layers I to III (the frame headers)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ 3,
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

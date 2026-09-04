// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPEG audio -- layers I, II and III, MPEG-1, 2 and 2.5 -- as a container, on
// libmpg123.
//
// **The container here is the frame headers**, which is a strange sentence
// until you notice that an `.mp3` has no container at all: it is a run of
// frames, each four bytes of header and then its own bytes, with tags stuck on
// either end. Finding where one ends and the next begins *is* the demultiplexing,
// and nothing else about the file is structure.
//
// **This is not `demux_mpeg` and the name is deliberate.** MPEG-1 System
// Streams and MPEG-2 Program Streams (`.mpg`, `.vob`) and Transport Streams
// (`.ts`) are multiplexed containers with pack headers, PES packets and
// 188-byte cells; they share nothing with this but a committee. Nothing here
// reads one and nothing here should: `demux_mpeg`, `demux_ps` and `demux_ts`
// are free names for when §9's video path wants them. What this module reads is
// an MPEG audio *elementary stream*, which is what RFC 3551 and everyone else
// calls MPA.
//
// **Why libmpg123 rather than the parser that used to be here.** The same
// reason `demux_flac` is on libFLAC and `demux_mp4` is on Bento4: where the
// people who define a format ship a reader for it, this tree reads the format
// with theirs. mpg123 documents `mpg123_framebyframe_next` and
// `mpg123_framedata` for exactly this -- *"together with the raw header, you
// can reconstruct the whole raw MPEG stream without junk and meta data"* -- so
// walking frames without decoding them is a supported use rather than a trick.
// It also brings the ID3v2 skip, the resynchronisation, the Xing/Info/LAME tag
// and an accurate frame index, which were four separate pieces of code here.
//
// **The file is opened by this module, not by mpg123.** `mpg123_reader64`
// installs our own read and seek over a `FILE*` from `open_utf8`, the way every
// other module in this tree opens a file. That is not only for consistency:
// mpg123 1.33.7 fixed heap buffer overflows in its Windows Unicode path
// conversion, and this is the arrangement under which that code is never
// reached at all.
//
// **Seeking is exact.** mpg123 keeps a frame index, so a seek goes to the frame
// that holds the target sample rather than near it -- and two frames before it,
// because MP3's bit reservoir reaches backwards.

#include <mediaperch/module.h>

#include <mpg123.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

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

// --------------------------------------------------------------- our I/O
//
// The 64-bit reader interface, `mpg123_reader64`, rather than the `off_t` one:
// on MSVC `off_t` is 32 bits and mpg123 renames its symbols around that, which
// is a trap that only shows up on files past two gigabytes. `int64_t` is
// `int64_t` everywhere.

int io_read(void* handle, void* buffer, std::size_t count, std::size_t* got) noexcept
{
    auto* fp = static_cast<FILE*>(handle);
    const std::size_t n = std::fread(buffer, 1, count, fp);
    if (got != nullptr) {
        *got = n;
    }
    // Short of a read error, a short read is the end of the file, which mpg123
    // reads as end of stream rather than as a failure.
    return std::ferror(fp) != 0 ? -1 : 0;
}

std::int64_t io_seek(void* handle, std::int64_t offset, int whence) noexcept
{
    auto* fp = static_cast<FILE*>(handle);
    if (_fseeki64(fp, offset, whence) != 0) {
        return -1;
    }
    return _ftelli64(fp);
}

// ------------------------------------------------------------ the format

/// Which layer this is, as an MpCodec. mpg123 counts layers 1, 2, 3 the way
/// people do rather than the way the bitstream field does.
MpCodec codec_for_layer(int layer) noexcept
{
    switch (layer) {
    case 1:
        return MP_CODEC_MP1;
    case 2:
        return MP_CODEC_MP2;
    case 3:
        return MP_CODEC_MP3;
    default:
        return MP_CODEC_UNKNOWN;
    }
}

/// The decoder's own latency, in samples, which the LAME tag's delay does not
/// include and every player has to add.
///
/// 529 is not this decoder's number, it is the format's: the synthesis
/// filterbank and the alias reconstruction together hold that many samples
/// before the first real one comes out, and LAME writes its delay on the
/// assumption that a decoder adds it. The C++ that preceded this file used the
/// same constant against dr_mp3 and the measurement did not move when the
/// decoder changed, which is the evidence that it belongs to the format.
constexpr std::uint64_t k_decoder_delay = 529;

/// Frames handed back before the one the caller asked for, after a seek.
///
/// **MP3's bit reservoir reaches backwards.** A frame may store part of its
/// audio in up to 511 bytes of the frames before it, and the synthesis
/// filterbank carries half a window across the boundary as well, so a decoder
/// started cold on the target frame is audibly wrong for the first one. Two
/// frames is what the format's own limit asks for; the host decodes them and
/// discards them, which is what MP_PACKET_TIMED is for.
constexpr std::int64_t k_preroll_frames = 2;

/// How many bytes of an ID3v2 tag's declared length are believed. A tag is
/// allowed to be large; a claim past this is not evidence of anything.
constexpr std::size_t k_max_tag = 4u << 20;

/// Whether four bytes are an MPEG audio frame header.
bool is_frame_header(const std::uint8_t* h) noexcept
{
    const bool sync = h[0] == 0xFFu && (h[1] & 0xE0u) == 0xE0u;
    const unsigned version = (h[1] >> 3) & 0x3u;
    const unsigned layer = (h[1] >> 1) & 0x3u;
    const unsigned bitrate = (h[2] >> 4) & 0xFu;
    const unsigned rate = (h[2] >> 2) & 0x3u;
    return sync && version != 1u && layer != 0u && bitrate != 0u && bitrate != 0xFu &&
           rate != 3u;
}

/// An ID3v2 tag's total length, header included, from its ten-byte header.
/// False when those bytes are not one, or claim a length nothing would write.
bool id3v2_length(const std::uint8_t* head, std::size_t& bytes) noexcept
{
    bytes = 0;
    if (std::memcmp(head, "ID3", 3) != 0) {
        return false;
    }
    const std::size_t size = (static_cast<std::size_t>(head[6] & 0x7Fu) << 21) |
                             (static_cast<std::size_t>(head[7] & 0x7Fu) << 14) |
                             (static_cast<std::size_t>(head[8] & 0x7Fu) << 7) |
                             static_cast<std::size_t>(head[9] & 0x7Fu);
    if (size > k_max_tag) {
        return false;
    }
    bytes = 10 + size;
    return true;
}

/// Whether `head` begins an MPEG audio elementary stream: an ID3v2 tag, if
/// there is one, and then a frame header where the audio should start.
///
/// **One sync pattern is not evidence.** Eleven set bits turn up in anything,
/// so the fields around them have to describe a frame as well: a real version,
/// a real layer, a bitrate that is not the reserved "free" or "bad" value, and
/// a sampling rate that exists.
///
/// This is `probe`'s test and `open`'s, and it has to be both. **libmpg123
/// resynchronises**, which is right for a broken MP3 and wrong as an answer to
/// "is this an MP3": measured, `mpg123_scan` finds frames inside the PCM of a
/// WAV, inside an M4A and inside a Matroska, and this module would then claim
/// to have opened all three. The probe already declined them; a demuxer that
/// probes 0 and opens anyway is one `--decoder` away from being believed.
bool starts_with_frame(const std::uint8_t* head, std::size_t bytes,
                       std::size_t& tag_bytes) noexcept
{
    tag_bytes = 0;
    if (head == nullptr || bytes < 10) {
        return false;
    }
    std::size_t at = 0;
    if (id3v2_length(head, tag_bytes)) {
        at = tag_bytes;
    } else if (std::memcmp(head, "ID3", 3) == 0) {
        return false; // a tag claiming a length nothing would write
    }
    if (at + 4 > bytes) {
        return false; // the header is past what the caller handed over
    }
    return is_frame_header(head + at);
}

} // namespace

struct MpDemux {
    FILE* fp = nullptr;
    mpg123_handle* mh = nullptr;

    MpCodec codec = MP_CODEC_UNKNOWN;
    MpFormat format{};
    std::uint32_t spf = 0; ///< PCM frames per MPEG frame
    std::uint64_t total_frames = 0;
    std::uint64_t skip_frames = 0;
    std::uint64_t play_frames = 0;

    /// The first frame's header, which is this format's whole configuration.
    std::uint8_t header[4] = {0, 0, 0, 0};

    /// Set once the last frame has been handed over, so a second `read_packet`
    /// says MP_END rather than asking mpg123 to parse past the end again.
    bool at_end = false;

    /// A frame that was parsed but did not fit the caller's buffer. It stays
    /// inside mpg123's handle, so the next `read_packet` must hand back the
    /// same one rather than parse the next -- otherwise a host that grows its
    /// buffer and asks again loses a frame, silently.
    bool pending = false;
    /// The frame index that `pending` frame sits at.
    std::int64_t at = 0;
};

namespace {

/// Everything mpg123 owns, released in the order it was taken.
void shut_down(MpDemux* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->mh != nullptr) {
        mpg123_close(d->mh);
        mpg123_delete(d->mh);
        d->mh = nullptr;
    }
    if (d->fp != nullptr) {
        std::fclose(d->fp);
        d->fp = nullptr;
    }
}

/// Opens `path` and points a fresh mpg123 handle at it, parsed as far as the
/// first frame. The caller owns `d` either way.
bool start(MpDemux* d, const char* path) noexcept
{
    d->fp = open_utf8(path);
    if (d->fp == nullptr) {
        return false;
    }

    int err = MPG123_OK;
    d->mh = mpg123_new(nullptr, &err);
    if (d->mh == nullptr) {
        return false;
    }

    // Quiet, because this module does the talking; and gapless off, because the
    // *host* applies the edit -- `MpStreamInfo` carries it and §4 says a codec
    // never sees it. With gapless on, `mpg123_length64` would come back already
    // trimmed and the numbers below would be applied twice.
    // **ID3v2 is skipped rather than parsed, and that is a memory bound.**
    // `store_id3v2` allocates a tag's *declared* length before reading it, so
    // a ten-byte header claiming 147 MB gets 147 MB -- found by `mpa_fuzzer`
    // in a few thousand executions, out of a file of a few hundred bytes. This
    // module reads no tags: `demux_mpa` wants frame boundaries and the LAME
    // gapless numbers, and neither comes from ID3. Skipping is free here and
    // removes the whole class.
    mpg123_param2(d->mh, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_SKIP_ID3V2, 0.0);
    mpg123_param2(d->mh, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0.0);

    if (mpg123_reader64(d->mh, io_read, io_seek, nullptr) != MPG123_OK) {
        return false;
    }
    return mpg123_open_handle64(d->mh, d->fp) == MPG123_OK;
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

    std::size_t tag = 0;
    if (starts_with_frame(head, bytes, tag)) {
        *out_score = 100;
    } else if (tag != 0 && tag + 4 > bytes) {
        // The ID3v2 tag is larger than the window a probe is given, so the frame
        // header is out of reach. Claim it weakly and let `open` decide, which
        // is allowed to read the whole file.
        *out_score = 60;
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
    // **The same test the probe applies, where the whole file is in reach.**
    // A probe sees four kilobytes and may have to guess past a large tag; this
    // does not.
    {
        FILE* peek = open_utf8(path);
        if (peek == nullptr) {
            delete d;
            return MP_ERR_IO;
        }
        // **Tags come in runs.** An ID3v2 tag may be followed by another one --
        // a tagger that appends rather than rewrites leaves two, and the format
        // allows it -- so this walks them rather than skipping one. Four is
        // past anything real and bounds a file that claims tags forever.
        bool ok = false;
        std::int64_t at = 0;
        for (int tags = 0; tags < 4; ++tags) {
            std::uint8_t head[10] = {};
            if (_fseeki64(peek, at, SEEK_SET) != 0 ||
                std::fread(head, 1, sizeof(head), peek) != sizeof(head)) {
                break;
            }
            if (is_frame_header(head)) {
                ok = true;
                break;
            }
            std::size_t tag = 0;
            if (!id3v2_length(head, tag)) {
                break; // not a tag and not a frame: not this module's file
            }
            at += static_cast<std::int64_t>(tag);
        }
        std::fclose(peek);
        if (!ok) {
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
    }

    if (!start(d, path)) {
        const MpResult why = d->fp == nullptr ? MP_ERR_IO : MP_ERR_UNSUPPORTED;
        shut_down(d);
        delete d;
        return why;
    }

    // **The whole file, once.** `mpg123_scan` walks every frame and builds the
    // index a seek needs and the length a player shows. It costs one pass over
    // the headers -- a few hundred kilobytes read for an album track -- and it
    // is the difference between an exact seek and an estimate. The parser this
    // replaced did the same walk by hand.
    if (mpg123_scan(d->mh) != MPG123_OK) {
        log_fmt(MP_LOG_DEBUG, "%s: %s", path, mpg123_strerror(d->mh));
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    mpg123_frameinfo2 info{};
    if (mpg123_info2(d->mh, &info) != MPG123_OK) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->codec = codec_for_layer(info.layer);
    if (d->codec == MP_CODEC_UNKNOWN || info.rate <= 0) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    const int spf = mpg123_spf(d->mh);
    if (spf <= 0) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->spf = static_cast<std::uint32_t>(spf);

    // What the *container* states. The depth is the codec's -- MPEG audio has
    // none of its own, and every decoder here produces float -- so only the
    // rate and the channel count are set, and `PacketSource` lets the codec's
    // answer decide the rest.
    d->format.sample_rate = static_cast<std::uint32_t>(info.rate);
    d->format.channels = (info.mode == MPG123_M_MONO) ? 1u : 2u;
    d->format.encoding = MP_ENCODING_PCM;

    const std::int64_t length = mpg123_length64(d->mh);
    d->total_frames = length > 0 ? static_cast<std::uint64_t>(length) : 0;

    // **The gapless edit, from the Xing/Info/LAME tag.** mpg123 parses it and
    // hands over the two numbers; -1 means the file did not say, which is most
    // MP2 and every layer I.
    //
    // The delay the tag states is the *encoder's*, and a decoder adds its own
    // 529 samples on top -- see k_decoder_delay. The padding at the end is
    // stated including that same 529, so it comes back off.
    long delay = -1;
    long padding = -1;
    double ignored = 0.0;
    (void)mpg123_getstate2(d->mh, MPG123_ENC_DELAY, &delay, &ignored);
    (void)mpg123_getstate2(d->mh, MPG123_ENC_PADDING, &padding, &ignored);
    if (delay >= 0 && padding >= 0) {
        const auto encoder_delay = static_cast<std::uint64_t>(delay);
        const auto stated_padding = static_cast<std::uint64_t>(padding);
        d->skip_frames = encoder_delay + k_decoder_delay;
        const std::uint64_t tail =
            stated_padding > k_decoder_delay ? stated_padding - k_decoder_delay : 0;
        if (d->total_frames > d->skip_frames + tail) {
            d->play_frames = d->total_frames - d->skip_frames - tail;
        }
    }

    // Back to the first frame, and take its header: that is this format's whole
    // configuration blob, and a codec is opened on it before a packet arrives.
    // **MPG123_NEW_FORMAT is a message, not a failure**, and it arrives on the
    // very first frame of every file: mpg123 is saying that the output format is
    // now known. A `< 0` test rejects it, which is what made this module decline
    // every MP3 it was handed until the code was measured.
    //
    // The order matters: `mpg123_framedata` describes the frame the last
    // `mpg123_framebyframe_next` parsed, so the seek has to come first.
    if (mpg123_seek_frame64(d->mh, 0, SEEK_SET) < 0) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    const int first = mpg123_framebyframe_next(d->mh);
    if (first != MPG123_OK && first != MPG123_NEW_FORMAT) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    unsigned long raw = 0;
    if (mpg123_framedata(d->mh, &raw, nullptr, nullptr) != MPG123_OK) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }
    d->header[0] = static_cast<std::uint8_t>((raw >> 24) & 0xFFu);
    d->header[1] = static_cast<std::uint8_t>((raw >> 16) & 0xFFu);
    d->header[2] = static_cast<std::uint8_t>((raw >> 8) & 0xFFu);
    d->header[3] = static_cast<std::uint8_t>(raw & 0xFFu);
    // **A fresh handle to walk with, and not a scanned one.**
    //
    // `mpg123_scan` leaves the reader at the end of the file, and a
    // frame-by-frame walk started from there loses the last frame: the walk
    // ends one short, and `mpg123_framebyframe_next` says MPG123_DONE on the
    // call that would have parsed it. Measured on a 78-frame file: 77 frames
    // out, 87599 samples where FFmpeg gives 88200 -- the last 26 milliseconds
    // of every track, silently. A seek back to frame zero does not undo it.
    //
    // Reopening does. The scan's numbers -- the length and the gapless tag --
    // are already read by then, and seeking after this uses the index mpg123
    // builds as it goes.
    mpg123_close(d->mh);
    if (mpg123_open_handle64(d->mh, d->fp) != MPG123_OK) {
        shut_down(d);
        delete d;
        return MP_ERR_UNSUPPORTED;
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
    if (d->at_end) {
        return MP_END;
    }

    // Where this frame starts, taken before it is parsed: `mpg123_tellframe64`
    // afterwards would name the *next* one.
    if (!d->pending) {
        d->at = mpg123_tellframe64(d->mh);
        const int status = mpg123_framebyframe_next(d->mh);
        if (status != MPG123_OK && status != MPG123_NEW_FORMAT) {
            // **The last frame and the end of the stream arrive together.**
            // `mpg123_framebyframe_next` returns MPG123_DONE on the call that
            // parses the final frame, not on the one after it -- the position
            // has moved past that frame by the time it says so. Taking DONE as
            // "nothing more" drops it: measured, 77 frames of a 78-frame file,
            // which is the last 26 milliseconds of every track, silently.
            //
            // So the walk ends here either way, and whether there is a frame to
            // hand over is decided by whether the position actually advanced.
            d->at_end = true;
            if (mpg123_tellframe64(d->mh) <= d->at) {
                return MP_END;
            }
            // MPG123_NEW_FORMAT is a message rather than a failure -- see
            // `start` -- and anything else here means a final frame that was
            // parsed as the stream ended, which is audio and belongs to the
            // caller.
        }
    }

    unsigned long raw = 0;
    unsigned char* body = nullptr;
    std::size_t body_bytes = 0;
    if (mpg123_framedata(d->mh, &raw, &body, &body_bytes) != MPG123_OK) {
        d->at_end = true;
        return MP_END;
    }

    // The packet is the header and the body, which is the frame as it sits in
    // the file. mpg123 hands them over separately because it keeps the header
    // parsed; putting them back together is four bytes of work.
    const std::size_t length = 4 + body_bytes;
    if (dst == nullptr || dst_bytes < length) {
        // **Nothing is consumed.** The frame stays parsed inside the handle, so
        // asking again with a larger buffer returns this same frame -- which is
        // why `at_end` is not set and the position is not moved.
        out->bytes = static_cast<std::uint32_t>(length);
        d->pending = true;
        return MP_ERR_NO_MEMORY;
    }

    auto* bytes = static_cast<std::uint8_t*>(dst);
    bytes[0] = static_cast<std::uint8_t>((raw >> 24) & 0xFFu);
    bytes[1] = static_cast<std::uint8_t>((raw >> 16) & 0xFFu);
    bytes[2] = static_cast<std::uint8_t>((raw >> 8) & 0xFFu);
    bytes[3] = static_cast<std::uint8_t>(raw & 0xFFu);
    if (body_bytes != 0 && body != nullptr) {
        std::memcpy(bytes + 4, body, body_bytes);
    }

    d->pending = false;
    out->bytes = static_cast<std::uint32_t>(length);
    out->frame = (d->at >= 0 ? static_cast<std::uint64_t>(d->at) : 0) * d->spf;
    // **No MP_PACKET_SYNC**, and that is the honest answer: a Layer III frame
    // may keep part of its audio in the frames before it, so it cannot be
    // decoded alone. `seek` is what does something about that.
    out->stream = 0; // the only one this container has
    out->flags = MP_PACKET_TIMED;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint32_t stream,
                            std::uint64_t frame) noexcept
{
    if (d == nullptr || stream != 0 || d->spf == 0) {
        return MP_ERR_INVALID;
    }
    const auto wanted = static_cast<std::int64_t>(frame / d->spf);
    const std::int64_t start = wanted > k_preroll_frames ? wanted - k_preroll_frames : 0;

    // **Reopened, then sought, so that a seek lands where an open lands.**
    // `mpg123_seek_frame64` on a handle that has been walked does not leave it
    // in the state a fresh open would, and the first frames afterwards decode
    // differently: measured on a seek to frame 1000, 2510 samples of the first
    // five frames disagreed with the same frames of a straight decode, and the
    // rest of the track agreed exactly. Reopening costs one `open_handle64` --
    // no scan, no pass over the file -- and makes the two paths the same path.
    mpg123_close(d->mh);
    if (mpg123_open_handle64(d->mh, d->fp) != MPG123_OK) {
        return MP_ERR_IO;
    }
    if (start != 0 && mpg123_seek_frame64(d->mh, start, SEEK_SET) < 0) {
        return MP_ERR_IO;
    }
    d->at_end = false;
    d->pending = false;
    return MP_OK;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
    shut_down(d);
    delete d;
}

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    // mpg123's one global: a table setup that is idempotent and, since 1.27,
    // done automatically by mpg123_new. Called anyway because the header still
    // documents it as the polite thing to do.
    return mpg123_init() == MPG123_OK ? MP_OK : MP_ERR_INTERNAL;
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
    /* read_frames   */ nullptr, // it splits properly, so it does not decode
    /* close         */ &demux_close,
    /* stream_video_info */ nullptr, // MPEG audio is audio
};

/// What it can produce. All three layers, because all three are the same
/// container and one of them is not more of a file format than the others.
const MpCodec g_codecs[] = {MP_CODEC_MP1, MP_CODEC_MP2, MP_CODEC_MP3};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 2, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 105,
    /* id          */ "demux_mpa",
    /* name        */ "MPEG audio, layers I to III (libmpg123)",
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

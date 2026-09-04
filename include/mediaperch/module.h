/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MediaPerch module ABI, version 2.
 *
 * The only file a third-party module has to read, and the only place where two
 * languages meet. Everything here is deliberately restricted to the C11 common
 * subset of C and C++:
 *
 *   - no fixed-underlying-type enums (`enum E : uint32_t`) -- C23 has them and
 *     MSVC 19.51 /std:clatest still does not, and this header must compile with
 *     a toolchain we do not control. `typedef uint32_t` plus untyped enumerators
 *     gives the same guaranteed 32-bit field everywhere;
 *   - no `bool`, no `nullptr`, no `static_assert` keyword, same reason;
 *   - no allocation across the boundary: the caller owns every buffer;
 *   - nothing that can unwind. C++ implementations use `noexcept` shims, Rust
 *     implementations wrap their bodies in `catch_unwind`.
 *
 * What actually guarantees the layout is the MP_STATIC_ASSERT block at the end
 * of this file, not the language version.
 *
 * Thread classes appear on every entry point and are part of the contract:
 *
 *   MP_RT   may not block, allocate, or unwind. Called from the render thread,
 *           which runs under MMCSS "Pro Audio" at a period that can be under 3 ms.
 *   MP_IO   may block and allocate. Called from the decode thread.
 *   MP_ANY  may block and allocate. Called from the control thread, at graph
 *           rebuild points only.
 */
#ifndef MEDIAPERCH_MODULE_H
#define MEDIAPERCH_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Portability shims                                                   */
/* ------------------------------------------------------------------ */

/* One calling convention, stated rather than assumed. x64 has only one, but
 * saying so is what makes a 32-bit build and a non-C++ implementation agree. */
#if defined(_WIN32)
#  define MP_CALL __cdecl
#else
#  define MP_CALL
#endif

#if defined(_WIN32)
#  define MP_EXPORT __declspec(dllexport)
#else
#  define MP_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
#  define MP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define MP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  define MP_STATIC_ASSERT(cond, msg) /* pre-C11: layout is checked by the host */
#endif

/* 2: containers and codecs are separate kinds. v1 had one MP_KIND_DECODER that
 * was both, and asked every one of them whether it could read a file; it is
 * gone, and so is every module that used it. See docs/plan.md §4, *ABI v2: the
 * container decides*. */
#define MP_ABI_VERSION 3u

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

typedef uint32_t MpResult;
enum {
    MP_OK = 0u,
    MP_END = 1u,              /* end of stream. Not an error. */
    MP_ERR_INVALID = 2u,      /* a caller passed nonsense */
    MP_ERR_UNSUPPORTED = 3u,  /* this module does not do that */
    MP_ERR_FORMAT = 4u,       /* the sink refused this format -- see negotiate */
    MP_ERR_IO = 5u,
    MP_ERR_DEVICE_LOST = 6u,  /* rebuild the graph */
    MP_ERR_BUSY = 7u,         /* another process holds the device exclusively */
    MP_ERR_DENIED = 8u,       /* exclusive mode disabled for this device */
    MP_ERR_NO_MEMORY = 9u,
    MP_ERR_INTERNAL = 10u,
    MP_TIMEOUT = 11u          /* a wait expired. Also not an error by itself. */
};

/* ------------------------------------------------------------------ */
/* Formats                                                             */
/* ------------------------------------------------------------------ */

typedef uint32_t MpSampleType;
enum {
    MP_SAMPLE_NONE = 0u,
    MP_SAMPLE_S16 = 1u,        /* 16 bits in 2 bytes */
    MP_SAMPLE_S24_PACKED = 2u, /* 24 bits in 3 bytes */
    MP_SAMPLE_S24_IN_32 = 3u,  /* 24 bits in 4 bytes, left-aligned */
    MP_SAMPLE_S32 = 4u,        /* 32 bits in 4 bytes */
    MP_SAMPLE_F32 = 5u,        /* IEEE 754 single. Path B only. */

    /* 8 bits in 1 byte, and the only UNSIGNED type here: silence is 128, not 0.
     * That is WAV's convention and it is not negotiable -- an 8-bit WAV really
     * does store unsigned samples -- so it gets its own type rather than being
     * quietly biased into S16 by whoever reads it. Path B only: no endpoint
     * takes 8-bit, and reaching one means a conversion. */
    MP_SAMPLE_U8 = 6u,

    /* IEEE 754 double. Path B only, and for the same reason as F32 with one
     * more: no audio hardware in existence accepts 64-bit samples, so this can
     * never be a wire format. It exists so that a decoder reading a 64-bit
     * float WAV can say what the file contains instead of narrowing it in
     * silence -- the narrowing then happens in the graph, where it is visible
     * and where somebody chose it. */
    MP_SAMPLE_F64 = 7u
};

typedef uint32_t MpEncoding;
enum {
    MP_ENCODING_PCM = 0u,
    MP_ENCODING_DOP = 1u,      /* DSD carried in 24-bit PCM frames (0x05/0xFA) */
    MP_ENCODING_IEC61937 = 2u  /* a compressed bitstream, for a receiver to decode */
};

/* Standard speaker positions, matching KSAUDIO's SPEAKER_* bits so that a
 * Windows sink can pass the mask straight through. */
enum {
    MP_SPEAKER_FRONT_LEFT = 0x1u,
    MP_SPEAKER_FRONT_RIGHT = 0x2u,
    MP_SPEAKER_FRONT_CENTER = 0x4u,
    MP_SPEAKER_LOW_FREQUENCY = 0x8u,
    MP_SPEAKER_BACK_LEFT = 0x10u,
    MP_SPEAKER_BACK_RIGHT = 0x20u,
    MP_SPEAKER_SIDE_LEFT = 0x200u,
    MP_SPEAKER_SIDE_RIGHT = 0x400u
};

/*
 * A format, as understood by both ends of the graph.
 *
 * `valid_bits` records how many bits actually carry signal, which is what makes
 * a widened candidate honest: 16-bit content offered to a 24-in-32 endpoint has
 * sample_type = MP_SAMPLE_S24_IN_32 and valid_bits = 16, and the host can still
 * say truthfully that nothing was lost.
 *
 * No `size` field: this is a value passed inside other structs, so it grows into
 * `reserved` instead.
 */
typedef struct MpFormat {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t channel_mask; /* 0 = unspecified; the non-extensible form */
    MpSampleType sample_type;
    MpEncoding encoding;
    uint32_t valid_bits; /* 0 = all of the container */
    uint32_t reserved[2];
} MpFormat;

/* ------------------------------------------------------------------ */
/* The host, as a module sees it                                       */
/* ------------------------------------------------------------------ */

typedef uint32_t MpLogLevel;
enum { MP_LOG_ERROR = 0u, MP_LOG_WARN = 1u, MP_LOG_INFO = 2u, MP_LOG_DEBUG = 3u };

typedef struct MpHost {
    uint32_t size;
    uint32_t reserved;
    void *ctx;
    /* MP_ANY. `msg` is UTF-8 and is not retained after the call returns. */
    void(MP_CALL *log)(void *ctx, MpLogLevel level, const char *msg);
    /* MP_ANY. Never called from an MP_RT entry point, by either side. */
    void *(MP_CALL *alloc)(void *ctx, size_t bytes);
    void(MP_CALL *release)(void *ctx, void *p);
} MpHost;

/* ------------------------------------------------------------------ */
/* Containers and codecs                                               */
/* ------------------------------------------------------------------ */

/* **The container decides.** A file is identified, opened, and asked what is in
 * it; each stream names its codec; the codec is looked up. Nothing is tried.
 *
 * v1 asked every decoder "can you read this file?" and tried the ones that said
 * yes. That question has no answer for a Matroska with video, two audio tracks
 * and three subtitle tracks -- and it had a poor one for MP4, where the box
 * naming the codec is in `moov` and `moov` may be at the end of a file a probe
 * sees four kilobytes of. See docs/plan.md §4, *ABI v2: the container decides*.
 *
 * The split adds no conversion. Bit-exactness is a property of a codec and a
 * container has none of it to lose. */

typedef uint32_t MpStreamKind;
enum {
    MP_STREAM_AUDIO = 1u,
    MP_STREAM_VIDEO = 2u,
    MP_STREAM_SUBTITLE = 3u,
    /* A stream this build has no name for. It is still counted and still
     * described, because a host that hides what it does not understand is a
     * host that cannot tell you why a track is missing. */
    MP_STREAM_OTHER = 4u
};

/* Codec identifiers.
 *
 * Ours rather than a container's: MP4 says `alac`, Matroska says `A_FLAC`, Ogg
 * says nothing and puts an identification header in the first page. A demuxer
 * maps its container's spelling onto these, which is the whole of what makes
 * the lookup a table rather than a guess.
 *
 * Numbered, not four-character-coded, because a fourcc invites a demuxer to
 * pass a container's bytes through unmapped -- and then two containers spelling
 * one codec differently become two codecs. */
typedef uint32_t MpCodec;
enum {
    MP_CODEC_UNKNOWN = 0u,

    /* Uncompressed. The "codec" is a memcpy or a container repack, which is an
     * honest description of what Path A already does. */
    MP_CODEC_PCM = 1u,
    MP_CODEC_DSD = 2u, /* reserved: DoP is designed for and not implemented */

    /* Lossless */
    MP_CODEC_FLAC = 16u,
    MP_CODEC_ALAC = 17u,
    MP_CODEC_WAVPACK = 18u,     /* reserved */
    MP_CODEC_APE = 19u,         /* reserved */
    MP_CODEC_TTA = 20u,         /* reserved */

    /* Lossy */
    MP_CODEC_MP1 = 32u,
    MP_CODEC_MP2 = 33u,
    MP_CODEC_MP3 = 34u,
    MP_CODEC_AAC_LC = 35u,
    MP_CODEC_HE_AAC = 36u,      /* reserved */
    MP_CODEC_VORBIS = 37u,
    MP_CODEC_OPUS = 38u,
    MP_CODEC_SPEEX = 39u,       /* reserved */
    MP_CODEC_WMA = 40u,         /* reserved */
    MP_CODEC_AC3 = 41u,         /* reserved */
    MP_CODEC_EAC3 = 42u,        /* reserved */
    MP_CODEC_DTS = 43u          /* reserved */
};

/* **What the configuration blob is, per codec.** A codec module is handed the
 * container's blob verbatim, so the two have to agree about what it contains,
 * and an ABI that left that to be discovered would not be one.
 *
 *   MP_CODEC_ALAC    the ALACSpecificConfig, 24 bytes big-endian.
 *   MP_CODEC_AAC_LC  the AudioSpecificConfig.
 *   MP_CODEC_OPUS    the OpusHead identification header, from its magic on.
 *   MP_CODEC_VORBIS  the three header packets -- identification, comment,
 *                    setup -- each preceded by its length as a 32-bit
 *                    little-endian integer. Vorbis is the one codec whose
 *                    configuration does not fit in a single blob, and every
 *                    container that carries it invents a framing; this one is
 *                    written down rather than guessed at.
 *   MP_CODEC_FLAC    the STREAMINFO block, without its metadata-block header.
 *   MP_CODEC_PCM     empty: MpStreamInfo::format is the whole of it.
 *
 * A codec that is handed a blob it does not recognise refuses in `open`, which
 * is why `probe` is given the blob too. */

/* The demuxer will decode this stream itself; there is no codec module to
 * look up. For a pipeline that cannot be split -- Media Foundation, FFmpeg
 * through its own programs -- and it is a declaration rather than a
 * disguise: such a module is a pipeline, and this says so.
 *
 * A stream with this codec always carries MP_STREAM_SELF_DECODES.
 *
 * A macro rather than an enumerator, and that is not a style choice: MSVC types
 * an unscoped enum as `int` regardless of its values, and clang does the same
 * in its MSVC-compatible mode, so 0xFFFFFFFF written inside the enum above
 * becomes -1 there, and a `switch` over an MpCodec will not compile against it.
 * The field is a uint32_t either way, so the value on the wire was never in
 * doubt -- only the constant's own type, and a macro has the type its suffix
 * says on every compiler. */
#define MP_CODEC_INTERNAL 0xFFFFFFFFu

enum {
    /* The demuxer decodes this stream. `MpDemuxVtbl::read_frames` is used
     * instead of a codec module. */
    MP_STREAM_SELF_DECODES = 1u << 0,
    /* The stream the container itself marks as the one to play. */
    MP_STREAM_DEFAULT = 1u << 1
};

/* What a container says about one stream.
 *
 * Everything here is what the *container* stated, not what a decoder inferred.
 * Where a container says nothing -- Ogg does not state a duration without
 * seeking -- the field is zero and zero means "not stated", never "none". */
typedef struct MpStreamInfo {
    uint32_t size;
    uint32_t index;
    MpStreamKind kind;
    MpCodec codec;
    uint32_t flags;
    uint32_t config_bytes; /* fetch with MpDemuxVtbl::stream_config */

    /* For an audio stream, as far as the container states it. A container that
     * states none leaves it zeroed and the codec's own configuration decides. */
    MpFormat format;

    /* Frames, in the stream's own rate, or 0 when the container does not say. */
    uint64_t total_frames;

    /* **The gapless edit, which is the container's and always was.** `elst` in
     * MP4, the LAME tag in an MP3's first frame, `pre_skip` in an Opus header.
     * v1 hid this inside three separate decoders; here a tagger or a video path
     * can see it too.
     *
     * `skip_frames` is the encoder delay to discard before the audio begins;
     * `play_frames` is how much to emit after that, or 0 when the file does not
     * say. */
    uint64_t skip_frames;
    uint64_t play_frames;

    /* Milliseconds, for a stream whose rate is not frames -- subtitles, and
     * video where the audio clock is what matters (§9). 0 when not stated. */
    uint64_t duration_ms;

    /* **Frames to drop from the end of the decoded stream, which is not the
     * same fact as `play_frames` and cannot be converted into it.**
     *
     * A container that knows the audible length states it, and `play_frames` is
     * where that goes: MP4's `elst` is a sample count, and an MP3's LAME tag is
     * a frame count. A container that knows only how much of the last frame was
     * padding states *that*, and it is this field: Matroska's `DiscardPadding`
     * and an Opus stream's final granule position both work that way.
     *
     * The two are not interchangeable, because turning a trim into a length
     * needs the decoded length, and a container that states a trim generally
     * cannot state that. Matroska is the case in point: its block timestamps
     * are scaled to milliseconds, so a length computed from the last block is
     * rounded, and rounding a lossless track's length is truncating it. The
     * trim, by contrast, is exact -- 13.5 ms of padding on a 48 kHz stream is
     * 648 frames however the timestamps were scaled.
     *
     * A host holds back this many frames and drops them when the stream ends.
     * Almost always 0. Where both this and `play_frames` are stated, the length
     * has already accounted for the padding and this is 0. */
    uint64_t trim_frames;

    uint32_t reserved[2];
} MpStreamInfo;

/* One packet, as it sits in the container. Whose memory it is is stated on
 * `read_packet` and it is the caller's, per §4 rule 1. */
typedef struct MpPacket {
    uint32_t size;
    uint32_t flags;
    /* Bytes actually written into the caller's buffer -- or, when the buffer
     * was too small, the bytes the packet needs. See `read_packet`. */
    uint32_t bytes;
    /* Which stream this packet belongs to. **v3**, in the slot v2 called
     * `reserved`: with several streams selected, a packet that could not say
     * where it came from would be a packet a host could only guess at. With one
     * selected it is that stream's index, every time. */
    uint32_t stream;
    /* Where this packet sits in its stream, **in that stream's own units and
     * in presentation order**, or 0 where the container does not timestamp --
     * a demuxer that cannot say must say 0 rather than guess, and clear
     * MP_PACKET_TIMED to mean it.
     *
     * **Presentation and not decode**, which matters only for video and
     * matters absolutely there: a stream with B-frames is stored in an order
     * that is not the order it is shown in, so the two numbers differ per
     * packet and only one of them can be compared against §8's audio clock.
     * Packets still arrive in storage order -- that is what a decoder wants
     * fed to it -- so `frame` is not monotonic for such a stream, and a host
     * that assumed it was would reorder a file that was already correct.
     *
     * The two containers that carry video here disagreed about this until it
     * was noticed: Matroska hands back a presentation timestamp and MP4 was
     * reporting a decode one, which is the same field meaning two things. See
     * plan.md §9.9.
     *
     * The units differ by what kind of stream it is, and each stream states
     * its own:
     *
     *  - **audio**: frames, at `MpStreamInfo::format.sample_rate` per second.
     *  - **video**: ticks, at `MpVideoInfo::timescale` per second. A video
     *    stream has no frames to count -- 24000/1001 of a second is not a unit
     *    anything divides evenly -- so the container's own tick is what it
     *    counts in, and `stream_video_info` is where it says how long one is.
     *
     * This used to say only "the stream's own frames", which an audio stream
     * has and a video stream does not, and the two demuxers that read video
     * answered differently: one declined to timestamp video at all and the
     * other handed back a number in a timescale nothing revealed. See
     * plan.md §9.9. */
    uint64_t frame;
} MpPacket;

enum {
    /* This packet begins a point the stream can be seeked to and decoded from
     * with no earlier packet. Every audio packet of most codecs is one; AAC and
     * MP3 are not, which is why seeking needs pre-roll. */
    MP_PACKET_SYNC = 1u << 0,

    /* `frame` is this packet's real position, and not a demuxer declining to
     * say. **Zero is a legitimate position**, so a host cannot tell "the start
     * of the stream" from "I do not timestamp" by looking at the number, and
     * after a seek the difference is the whole of whether the audio starts
     * where it was asked for: the host discards frames from `frame` up to the
     * target, and doing that on a number nobody vouched for would be worse than
     * not doing it at all.
     *
     * A demuxer that seeks to the packet containing a frame, or that seeks
     * further back to give a codec its pre-roll, has to set this -- both leave
     * audio in front of the target that only the host can discard. */
    MP_PACKET_TIMED = 1u << 1
};

typedef struct MpDemux MpDemux; /* opaque, module-owned */

/* What a container states about a video stream, for the renderer that has to
 * put it on a display of a kind the file knows nothing about.
 *
 * **The three code points are the reason this struct exists.** Width and height
 * a decoder reports anyway; `primaries`, `transfer` and `matrix` it does not,
 * because they are not in the bitstream of every codec and, where they are, the
 * container's copy is the one a muxer wrote deliberately. Whether a frame is
 * BT.709 or BT.2020, and whether its transfer is sRGB or PQ, decides whether it
 * is tone-mapped at all -- and a renderer that guesses gets a picture that is
 * merely plausible, which is the failure nobody files a bug about.
 *
 * The values are the code points ISO/IEC 23091-2 assigns -- the same tables
 * H.273, HEVC and AV1 use, and the same numbers FFmpeg's `AVColorPrimaries`
 * carries. **2 is "unspecified" in all three**, and is what a container that
 * says nothing leaves behind; a renderer then applies the convention for the
 * resolution, which is BT.709 for HD and BT.601 below it.
 *
 * Mastering-display metadata (ST.2086) and content light level are not here
 * yet. They are what §9.3's `driver` provider hands
 * `VideoProcessorSetStreamHDRMetaData`, and they append to the end of this
 * struct the day that provider is written -- a field nothing fills is a field
 * nothing checks. */
typedef struct MpVideoInfo {
    uint32_t size; /* set by the caller */
    uint32_t width;
    uint32_t height;

    /* Display size, when the container says the pixels are not square. Equal to
     * `width` and `height` otherwise, and never zero. */
    uint32_t display_width;
    uint32_t display_height;

    /* A ratio, because 24000/1001 is not a decimal and rounding it is how a
     * player drifts a frame every seventeen minutes. 0/0 when the container
     * does not say -- which is normal for a container that timestamps every
     * frame instead. */
    uint32_t fps_num;
    uint32_t fps_den;

    /* ISO/IEC 23091-2 code points; 2 is unspecified. */
    uint32_t primaries;
    uint32_t transfer;
    uint32_t matrix;

    uint32_t flags; /* MP_VIDEO_FULL_RANGE */

    /* Ticks per second of `MpPacket::frame` and of `seek`'s `frame`, for this
     * stream. **The unit the timestamps are in**, which is the container's own:
     * an MP4 track states it in `mdhd` and it is typically the frame rate's
     * numerator; Matroska stores a scale and libmatroska hands back
     * nanoseconds, so a Matroska stream reports 1000000000.
     *
     * 0 means the demuxer does not timestamp this stream, and every packet of
     * it comes back with MP_PACKET_TIMED clear.
     *
     * Ticks per second rather than a rational seconds-per-tick, because that is
     * the form both containers store and an integer cannot round it. It is the
     * reciprocal of what FFmpeg calls a time base, whose numerator is 1 for
     * both of them. */
    uint32_t timescale;
} MpVideoInfo;

enum {
    /* The samples use the full range of their container rather than the studio
     * range, which for 8-bit means 0..255 instead of 16..235. A container that
     * does not say leaves this clear, and studio range is the convention that
     * makes that safe. */
    MP_VIDEO_FULL_RANGE = 1u << 0
};

typedef struct MpDemuxVtbl {
    uint32_t size;
    uint32_t reserved;

    /* MP_IO. Score 0 = not this container, 100 = certain. `head` is the first
     * `head_bytes` of the file. **This is a question about the container only.**
     * What is inside it is not a probe's business and is not visible from four
     * kilobytes anyway. */
    MpResult(MP_CALL *probe)(const char *path, const uint8_t *head, size_t head_bytes,
                             uint32_t *out_score);

    /* MP_IO. Reads the container's index, wherever it is in the file. A demuxer
     * is not a probe and has the whole file. */
    MpResult(MP_CALL *open)(const char *path, MpDemux **out);

    /* MP_ANY */
    MpResult(MP_CALL *stream_count)(MpDemux *d, uint32_t *out_count);
    /* MP_ANY. `out->size` is set by the caller. */
    MpResult(MP_CALL *stream_info)(MpDemux *d, uint32_t index, MpStreamInfo *out);
    /* MP_ANY. The codec's configuration blob, verbatim: ALACSpecificConfig,
     * AudioSpecificConfig, a FLAC STREAMINFO. Fills at most `out_bytes` and
     * always reports what it needs, so a caller may ask with `out` NULL first. */
    MpResult(MP_CALL *stream_config)(MpDemux *d, uint32_t index, uint8_t *out,
                                     uint32_t out_bytes, uint32_t *out_needed);

    /* MP_IO. Which streams `read_packet` may return. **v3**: v2 had
     * `select(d, index)` and could name one, which is the whole of why this
     * broke -- a player showing video reads audio and video out of one file,
     * and reading them by opening the container twice means two file positions,
     * two indexes and two seeks that have to agree.
     *
     * `indices` is `count` stream indexes, in any order, and selecting again
     * replaces the set rather than adding to it. **`count` is at least 1**: a
     * host that has stopped reading closes the demuxer, and one turning off a
     * video track selects the audio stream rather than none, so an empty set
     * had no caller and saying it meant something would have been five modules
     * disagreeing with this sentence.
     *
     * A demuxer that cannot serve several at once answers MP_ERR_UNSUPPORTED
     * for a `count` above one -- which is a real answer, because most
     * containers here hold one stream and have nothing to interleave. */
    MpResult(MP_CALL *select_streams)(MpDemux *d, const uint32_t *indices, uint32_t count);

    /* MP_IO. Fills the caller's buffer with the next packet of any selected
     * stream, **in the order the container stores them**, and says which stream
     * it was in `out->stream`. MP_END with `out->bytes == 0` when every selected
     * stream is finished.
     *
     * Storage order and not a schedule: a host that wants audio and is handed
     * video keeps the video packet. That is the host's queue to own, and it is
     * the only arrangement that reads a file once.
     *
     * When the packet does not fit, nothing is consumed, `out->bytes` is what it
     * needs and the result is MP_ERR_NO_MEMORY -- so a host grows its buffer and
     * asks again rather than losing a packet it cannot hold. */
    MpResult(MP_CALL *read_packet)(MpDemux *d, void *dst, size_t dst_bytes, MpPacket *out);

    /* MP_IO. To the packet containing `frame` of stream `stream`, or the nearest
     * sync point before it. The host feeds what comes back to a codec that has
     * been reset, and discards what precedes `frame`.
     *
     * **v3 names the stream, and `frame` is in that stream's own units** --
     * the same ones `MpPacket::frame` uses, which are frames for an audio
     * stream and `MpVideoInfo::timescale` ticks for a video one. v2's
     * `seek(frame)` meant "the selected stream" and had no answer once two were
     * selected. `stream` need not be one of the selected ones: a host seeking by
     * the audio clock names the audio stream whether or not it is reading it.
     *
     * **One file has one position**, so this moves every selected stream, which
     * is what an interleaved container can do and all it can do. Each stream
     * then arrives from wherever its own nearest sync point was, and the host
     * discards what precedes its target -- per stream, because the points are
     * not the same point. */
    MpResult(MP_CALL *seek)(MpDemux *d, uint32_t stream, uint64_t frame);

    /* MP_IO. Only for a stream flagged MP_STREAM_SELF_DECODES: PCM in the format
     * `stream_info` reported. Null on a
     * demuxer that splits properly, which is most of them. */
    MpResult(MP_CALL *read_frames)(MpDemux *d, void *dst, size_t dst_bytes,
                                   size_t *out_bytes);

    void(MP_CALL *close)(MpDemux *d);

    /* --- v3, appended ------------------------------------------------- */

    /* MP_ANY. What the container says about a video stream: its geometry, and
     * the three code points a renderer cannot guess. MP_ERR_UNSUPPORTED for an
     * audio stream, and NULL on a demuxer with no video in it -- which is most
     * of them, and is why this is here rather than inside MpStreamInfo, whose
     * `format` field says "for an audio stream" and should go on saying it. */
    MpResult(MP_CALL *stream_video_info)(MpDemux *d, uint32_t index, MpVideoInfo *out);
} MpDemuxVtbl;

/* Named apart from `MpCodec`, which is an identifier rather than an object. The
 * two colliding was the first thing this header caught after v2 was written. */
typedef struct MpCodecInstance MpCodecInstance; /* opaque, module-owned */

typedef struct MpCodecVtbl {
    uint32_t size;
    uint32_t reserved;

    /* MP_ANY. Whether this module decodes `codec`, and how well. A codec module
     * never sees a file: this is a question about an identifier and a
     * configuration blob, answered from data. */
    MpResult(MP_CALL *probe)(MpCodec codec, const uint8_t *config, uint32_t config_bytes,
                             uint32_t *out_score);

    /* MP_IO. `config` is the container's blob, verbatim and possibly empty. */
    MpResult(MP_CALL *open)(MpCodec codec, const uint8_t *config, uint32_t config_bytes,
                            MpCodecInstance **out);

    /* MP_ANY. What this codec produces. Known after `open` for a codec whose
     * configuration states it, and after the first `decode` otherwise. */
    MpResult(MP_CALL *get_format)(MpCodecInstance *c, MpFormat *out);

    /* MP_IO. One packet in, PCM out, in the format `get_format` reports.
     * **Never converts**: conversion is the graph's job, here as in v1. A packet
     * that decodes to nothing -- a priming frame -- returns MP_OK with
     * *out_bytes == 0. */
    MpResult(MP_CALL *decode)(MpCodecInstance *c, const void *packet, size_t packet_bytes,
                              void *dst, size_t dst_bytes, size_t *out_bytes);

    /* MP_IO. What is still inside after the last packet. */
    MpResult(MP_CALL *flush)(MpCodecInstance *c, void *dst, size_t dst_bytes, size_t *out_bytes);

    /* MP_IO. Forget everything: the audio after a seek is not adjacent to the
     * audio before it. The host then feeds pre-roll packets before keeping any
     * output, which is where AAC's priming frame and MP3's bit reservoir live.
     * v1 hid that inside each decoder; here it is the host's, once. */
    MpResult(MP_CALL *reset)(MpCodecInstance *c);

    void(MP_CALL *close)(MpCodecInstance *c);
} MpCodecVtbl;

/* ------------------------------------------------------------------ */
/* Video presentation                                                  */
/* ------------------------------------------------------------------ */

/* **A presenter is to video what a sink is to audio**, and the resemblance is
 * deliberate: open on a device, tell it what is coming, hand it frames. What
 * does not carry over is negotiation. An audio sink may refuse a format and
 * §6's whole argument rests on it being able to; a display refuses nothing and
 * converts everything, which is why §9 is about *how* it converts rather than
 * about whether it will.
 *
 * The frames are on the CPU. A hardware decoder produces a texture and handing
 * one over without a copy is the point of decoding on the GPU -- but nothing
 * here decodes video yet, and a field no module fills is a field no test
 * checks. It appends when `codec_mft` arrives. */

typedef struct MpVideo MpVideo; /* opaque, module-owned */

typedef uint32_t MpPixelFormat;
enum {
    MP_PIXEL_NONE = 0u,
    /* 8-bit 4:2:0: a full-size Y plane, then a half-size interleaved UV plane.
     * What every hardware decoder on Windows produces. */
    MP_PIXEL_NV12 = 1u,
    /* The same two planes with 10 bits in the top of each 16, which is what
     * HDR content decodes to. */
    MP_PIXEL_P010 = 2u,
    /* One plane, 8 bits each of B, G, R and A. Not a codec's output -- it is
     * what a test pattern and a software decoder have, and what makes a
     * presenter checkable before there is anything to decode. */
    MP_PIXEL_BGRA8 = 3u,

    /* The three a presenter renders *into*, and hands back from `read_back`.
     * All are one plane, RGBA order, tightly packed.
     *
     * `RGBA16F` is scRGB as a display gets it: linear half-float, and **the
     * most precise format a flip-model swap chain accepts** -- DXGI offers
     * 8-bit UNORM, 10-bit UNORM and this, and nothing above it, so presenting
     * through the desktop is capped here by the platform rather than by a
     * choice. It is not a *sufficient* format: half's relative step is
     * 1/1024 at worst, and a 12-bit output needs 1/1706 at white. See
     * plan.md §9.10, and note that a presenter on a dedicated video output
     * would quantise from RGBA32F straight to the card's own integer format
     * and never touch half.
     *
     * `RGB10A2` is HDR10: PQ-encoded, ten bits a channel packed into a u32
     * with R in the low bits.
     *
     * `RGBA32F` is linear single precision, which a swap chain will not take
     * and an off-screen target will. It is what a *measurement* renders into,
     * so that what a test hashes is the pipeline's arithmetic and not the
     * presentation format's rounding. */
    MP_PIXEL_RGBA16F = 4u,
    MP_PIXEL_RGB10A2 = 5u,
    MP_PIXEL_RGBA32F = 6u
};

/* One frame, in the pixels a decoder produced.
 *
 * `pts` is in `MpVideoInfo::timescale` ticks, the same units the demuxer
 * timestamps packets in -- §8 compares it against the audio clock and drops or
 * duplicates against that, never the other way round. */
typedef struct MpVideoFrame {
    uint32_t size;
    MpPixelFormat format;
    uint32_t width;
    uint32_t height;
    /* NULL past what the format uses. */
    const void *plane[3];
    uint32_t stride[3];
    uint32_t reserved;
    uint64_t pts;
} MpVideoFrame;

typedef struct MpVideoVtbl {
    uint32_t size;
    uint32_t reserved;

    /* MP_ANY. `window` is an HWND on Windows. **NULL means off-screen**, which
     * is not a degraded mode: it is how a presenter is measured. Everything
     * §9 decides -- which transfer function, which tone mapper, how much to
     * scale SDR content by -- lands in pixels, and pixels can be hashed. A
     * renderer nobody can test is a renderer judged by whether it looks
     * plausible, which is how the OS tone mapper's 2.4 gamma survived for
     * years (§9.2). */
    MpResult(MP_CALL *open)(void *window, MpVideo **out);
    void(MP_CALL *close)(MpVideo *v);

    /* MP_ANY. What is coming: the geometry and the colour the container
     * stated. The presenter answers by choosing a swap chain format, a colour
     * space and a tone mapper, and `describe` says which. */
    MpResult(MP_CALL *configure)(MpVideo *v, const MpVideoInfo *in);

    /* MP_IO. One frame, presented. */
    MpResult(MP_CALL *present)(MpVideo *v, const MpVideoFrame *frame);

    /* MP_ANY. `key=value`, as MpDspVtbl means it, and the same `trouble`
     * convention for a stage that knows why it refused. */
    MpResult(MP_CALL *set)(MpVideo *v, const char *key, const char *value);
    /* MP_ANY. One `key\tcurrent\tdescription` per index, MP_END past the last. */
    MpResult(MP_CALL *describe)(MpVideo *v, uint32_t index, char *out,
                                uint32_t out_bytes);

    /* MP_ANY. The pixels last presented, **in the format they were rendered
     * in**, tightly packed. `out_format` says which -- MP_PIXEL_RGBA16F for
     * the scRGB path, MP_PIXEL_RGB10A2 for HDR10. Call with `dst` NULL to be
     * told the geometry and the format; the result is MP_ERR_NO_MEMORY and
     * nothing is written, the same shape `read_packet` uses.
     *
     * **This is the measuring apparatus, in the ABI on purpose.** A screenshot
     * is a feature people want; a rendered frame a test can hash is the only
     * way a colour pipeline gets held to anything. Both are the same call.
     *
     * And it does not convert, for the reason nothing else in this tree does.
     * An earlier version handed back 8-bit sRGB, which was wrong twice: it
     * quantised away the dark end, where the difference between the sRGB curve
     * and the 2.4 gamma of §9.2 actually lives and where a measurement is for;
     * and on the HDR10 path it read PQ code values as though they were linear,
     * producing bytes that were neither. A caller that wants eight bits knows
     * what it wants them for. */
    MpResult(MP_CALL *read_back)(MpVideo *v, void *dst, size_t dst_bytes,
                                 uint32_t *out_width, uint32_t *out_height,
                                 MpPixelFormat *out_format);
} MpVideoVtbl;

/* ------------------------------------------------------------------ */
/* Sinks                                                               */
/* ------------------------------------------------------------------ */

typedef struct MpSink MpSink; /* opaque, module-owned */

typedef uint32_t MpShareMode;
enum { MP_SHARE_EXCLUSIVE = 0u, MP_SHARE_SHARED = 1u };

enum {
    MP_DEVICE_IS_DEFAULT = 1u << 0,

    /* The endpoint has a volume control that the Windows audio engine does not
     * implement -- IAudioEndpointVolume::QueryHardwareSupport reports
     * ENDPOINT_HARDWARE_SUPPORT_VOLUME.
     *
     * Read that carefully, because it is weaker than it sounds and the flag is
     * named for what the API actually says. It means the control lives below
     * Windows. It does *not* say whether the driver applies it by scaling
     * samples -- which costs bits like any other gain -- or whether the hardware
     * applies it after the converter, which costs none. Windows will not tell
     * you which, and on this machine a virtual cable claims it as readily as a
     * USB DAC does.
     *
     * So this is a signal that a bit-exact volume control might be possible, not
     * that one is. Path A has no gain stage of its own and never will; the honest
     * behaviour for a device without this flag is to show no volume control at
     * all, rather than to move the stream quietly onto Path B. */
    MP_DEVICE_ENDPOINT_VOLUME = 1u << 1,

    /* Whether exclusive mode is permitted is a per-device user setting with no
     * documented way to read it. The only way to find out is to negotiate and
     * see whether MP_ERR_DENIED comes back, so this is set once that has
     * happened and is absent, not false, before then. */
    MP_DEVICE_EXCLUSIVE_PROVEN = 1u << 2
};

typedef struct MpDeviceInfo {
    uint32_t size;
    uint32_t flags;
    char id[256];   /* UTF-8, opaque, stable across sessions */
    char name[256]; /* UTF-8, for people */
} MpDeviceInfo;

/* Flags for MpSinkVtbl::commit. */
enum { MP_COMMIT_SILENT = 1u << 0 };

typedef struct MpSinkVtbl {
    uint32_t size;
    uint32_t reserved;

    /* MP_ANY. Returns MP_END when `index` is past the last device. */
    MpResult(MP_CALL *enumerate)(uint32_t index, MpDeviceInfo *out);
    /* MP_ANY. `device_id` NULL means the default endpoint. */
    MpResult(MP_CALL *open)(const char *device_id, MpShareMode mode, MpSink **out);

    /*
     * MP_ANY, and the reason this ABI exists.
     *
     * Actually initialises the underlying client -- IsFormatSupported is a hint
     * and drivers answer it optimistically, so a sink that only asks is a sink
     * that lies. On success `out_accepted` is what will really be played, which
     * may differ from `want` only in ways the host classifies as bit-exact.
     * On MP_ERR_FORMAT the sink is left closed-but-open and may be asked again
     * with a different format.
     */
    MpResult(MP_CALL *negotiate)(MpSink *s, const MpFormat *want, MpFormat *out_accepted);

    /* MP_ANY. Frames in one buffer period, valid after negotiate. */
    MpResult(MP_CALL *get_period)(MpSink *s, uint32_t *out_frames);

    MpResult(MP_CALL *start)(MpSink *s); /* MP_ANY */
    MpResult(MP_CALL *stop)(MpSink *s);  /* MP_ANY */
    void(MP_CALL *close)(MpSink *s);     /* MP_ANY */

    /* MP_RT. Blocks on the device's own event until a buffer is free. This is
     * the one MP_RT call permitted to wait, because waiting on the device is
     * how the render thread is paced. */
    MpResult(MP_CALL *wait)(MpSink *s, uint32_t timeout_ms);
    /* MP_RT. Hands out the device buffer to write into. */
    MpResult(MP_CALL *acquire)(MpSink *s, void **out_ptr, uint32_t *out_frames);
    /* MP_RT. Gives it back. `frames` must be what acquire reported. */
    MpResult(MP_CALL *commit)(MpSink *s, uint32_t frames, uint32_t flags);

    /* MP_ANY. The master clock: frames played, and the QPC tick it was read at. */
    MpResult(MP_CALL *get_position)(MpSink *s, uint64_t *out_frames, uint64_t *out_qpc);
} MpSinkVtbl;

/* ------------------------------------------------------------------ */
/* DSP                                                                 */
/* ------------------------------------------------------------------ */

/*
 * A stage in Path B's chain.
 *
 * Path A has no chain and never will: it exists so that a file can reach a
 * device without anything touching it. This is the other path, where things are
 * meant to touch it, and where a resampler, a ReplayGain, an equaliser or a
 * convolver are all the same shape of thing.
 *
 * The bus is **deinterleaved double**, one contiguous array per channel. Two
 * decisions worth stating:
 *
 *   - *Deinterleaved*, because that is what every filter anybody will write
 *     wants: a per-channel history, a per-channel coefficient state, and an
 *     inner loop that walks memory forwards.
 *   - *double*, not float. The plan of record said f32 and it was written
 *     before the converter was; that converter reads every sample exactly into
 *     binary64 and quantises once at the very end, and putting an f32 bus in
 *     the middle would add a rounding to a path whose whole argument is that it
 *     has exactly one.
 *
 * A stage may produce a different number of frames than it consumed, and may
 * report a different format from the one it was given -- that is what a
 * resampler is. `configure` is where it says so, before anything runs.
 *
 * Every call here is MP_ANY: the chain runs on the decode thread, never on the
 * render thread, exactly as the sample-type conversion does.
 */
typedef struct MpDsp MpDsp;

typedef struct MpDspVtbl {
    uint32_t size;
    uint32_t reserved;

    MpResult(MP_CALL *open)(MpDsp **out);
    void(MP_CALL *close)(MpDsp *d);

    /*
     * What this stage would produce, given what it is being handed.
     *
     * `max_frames` is the largest block `process` will be asked for, so a stage
     * can size its own buffers once here and allocate nothing afterwards.
     * `out_format` may differ from `in_format` in sample rate and channel
     * count; the host chains stages by feeding each one the previous one's
     * answer. A stage that cannot work with `in_format` returns MP_ERR_FORMAT
     * and is left out of the graph rather than run and hoped for.
     */
    MpResult(MP_CALL *configure)(MpDsp *d, const MpFormat *in_format, uint32_t max_frames,
                                 MpFormat *out_format, uint32_t *out_max_frames);

    /*
     * `in_frames` frames from `in`, up to `out_capacity` frames into `out`,
     * both deinterleaved: `in[c]` is channel c's samples. `out_frames` is how
     * many were produced, which may be zero while a stage fills its own
     * history and may exceed `in_frames` when a stage is upsampling.
     *
     * `in` may be NULL with `in_frames` zero, which is how a host asks a stage
     * to keep producing from what it already holds.
     */
    MpResult(MP_CALL *process)(MpDsp *d, const double *const *in, uint32_t in_frames,
                               double *const *out, uint32_t out_capacity,
                               uint32_t *out_frames);

    /*
     * The end of the stream: produce whatever is still inside. Called until it
     * reports zero frames. A stage with no memory can report zero immediately.
     */
    MpResult(MP_CALL *flush)(MpDsp *d, double *const *out, uint32_t out_capacity,
                             uint32_t *out_frames);

    /*
     * One setting, as text.
     *
     * Text because the shell is a separate process behind a versioned wire
     * format and must be able to drive a stage nobody had written when the
     * shell was built. A stage returns MP_ERR_UNSUPPORTED for a key it does not
     * have, so a host can offer what it finds rather than what it was compiled
     * against. NULL `key` with `index` semantics is not provided: enumeration
     * belongs in `describe`.
     */
    MpResult(MP_CALL *set)(MpDsp *d, const char *key, const char *value);

    /*
     * The settings this stage has, as one UTF-8 line per key:
     *   "key\tcurrent\tdescription\n"
     * Returns MP_END when `index` is past the last one. This is how `--dsp
     * list` and a settings dialogue both find out what a stage can do without
     * either of them knowing what the stage is.
     */
    MpResult(MP_CALL *describe)(MpDsp *d, uint32_t index, char *out, uint32_t out_bytes);

    /*
     * Forget everything about where the stream was, keeping the settings.
     *
     * What a seek needs. A resampler holds the samples either side of where it
     * is, a convolver holds a whole impulse response's worth, an equaliser
     * holds two numbers per section -- and after a seek every one of those
     * belongs to audio that is no longer adjacent to what comes next. Playing
     * them out is a click at best.
     *
     * Distinct from `configure`, which would also clear the state and would
     * additionally redesign the filter -- seconds of work for a convolver, to
     * throw away a few hundred samples. May be NULL for a stage that has no
     * memory, which is what `size` is for.
     */
    MpResult(MP_CALL *reset)(MpDsp *d);

    /* MP_ANY. How many frames of the *output* are older audio -- the delay this
     * stage adds between a sample going in and coming out, at the format
     * `configure` was given. 0 for a stage that delays nothing.
     *
     * **A number, not a sentence.** Three stages here have latency and until now
     * all three reported it only through `describe`, as text for a person: a
     * linear-phase equaliser delays by half its filter, a convolver by its
     * partition, a resampler by its own. Nothing could ask.
     *
     * Two things need to. A player's position is the device's, and with a
     * linear-phase stage in the chain what is *audible* is this many frames
     * behind what the position says. And an engine running several chains into
     * one bus -- a mixer, a DAW -- must delay the short chains to match the
     * long one, which is impossible without the number. Getting that wrong
     * moves tracks against each other, which is the one error in a mixer that
     * is never subtle.
     *
     * Valid after `configure` and may change with it: latency is a property of
     * the filter that was built, not of the module. */
    MpResult(MP_CALL *get_latency)(MpDsp *d, uint32_t *out_frames);
} MpDspVtbl;

/* ------------------------------------------------------------------ */
/* The module itself                                                   */
/* ------------------------------------------------------------------ */

typedef uint32_t MpKind;
enum {
    /* 1 was MP_KIND_DECODER: a container reader and a codec in one object, and
     * the reason the host used to try modules in order. It is not reused. An id
     * that meant something else once is an id a host can get wrong, and the
     * cost of leaving a hole is a hole. */
    MP_KIND_SINK = 2u,
    MP_KIND_DSP = 3u,   /* MpDspVtbl */
    MP_KIND_VIDEO = 4u, /* MpVideoVtbl */
    MP_KIND_META = 5u,  /* reserved */
    MP_KIND_DEMUX = 6u, /* MpDemuxVtbl */
    MP_KIND_CODEC = 7u  /* MpCodecVtbl */
};

enum {
    /* The module started threads, registered COM classes, or otherwise cannot be
     * FreeLibrary'd honestly. The host keeps it for the process lifetime rather
     * than pretending to unload it. */
    MP_MODULE_NO_UNLOAD = 1u << 0
};

#define MP_MAKE_VERSION(major, minor, patch) \
    (((uint32_t)(major) << 22) | ((uint32_t)(minor) << 12) | (uint32_t)(patch))

typedef struct MpModuleDesc {
    uint32_t size;
    uint32_t abi_version; /* MP_ABI_VERSION the module was built against */
    uint32_t flags;
    uint32_t version; /* MP_MAKE_VERSION */
    MpKind kind;
    /* Higher wins when several modules claim the same file. */
    uint32_t priority;

    const char *id;   /* stable, ASCII, e.g. "sink_wasapi" */
    const char *name; /* UTF-8, for people */

    /* MP_ANY. Called once after load, before anything else. The host vtable
     * outlives the module. */
    MpResult(MP_CALL *init)(const MpHost *host);
    /* MP_ANY. Called once before unload, after every object is closed. */
    void(MP_CALL *shutdown)(void);


    /* MpDemuxVtbl*, MpCodecVtbl*, MpSinkVtbl* or MpDspVtbl*, per `kind`.
     * Owned by the module and valid until shutdown returns. */
    const void *vtbl;

    /* **Capability declaration is data, not code** (§4 rule 6). What a module
     * claims, so the registry can build the resolution table without loading
     * and initialising everything at every start.
     *
     * For MP_KIND_CODEC: the MpCodec values it decodes. For MP_KIND_DEMUX: the
     * codecs it can produce, which is a hint for a report rather than a promise
     * -- a container carries what it carries. NULL and 0 mean "ask", which is
     * what every v1 module says by having been compiled before this existed. */
    const MpCodec *codecs;
    uint32_t codec_count;
    uint32_t reserved_desc;
} MpModuleDesc;

/*
 * The one exported symbol.
 *
 * Returns NULL if the module cannot work with this host -- which is the correct
 * answer for an ABI mismatch, and the only thing a module may do about one.
 */
typedef const MpModuleDesc *(MP_CALL *MpModuleEntry)(uint32_t host_abi_version);

MP_EXPORT const MpModuleDesc *MP_CALL mp_module_entry(uint32_t host_abi_version);

#define MP_MODULE_ENTRY_NAME "mp_module_entry"

/* ------------------------------------------------------------------ */
/* Layout, asserted rather than assumed                                */
/* ------------------------------------------------------------------ */

MP_STATIC_ASSERT(sizeof(uint32_t) == 4, "u32");
MP_STATIC_ASSERT(sizeof(MpResult) == 4, "MpResult is a u32 field");
MP_STATIC_ASSERT(sizeof(MpSampleType) == 4, "MpSampleType is a u32 field");
MP_STATIC_ASSERT(sizeof(MpEncoding) == 4, "MpEncoding is a u32 field");
MP_STATIC_ASSERT(sizeof(MpKind) == 4, "MpKind is a u32 field");
MP_STATIC_ASSERT(offsetof(MpDspVtbl, size) == 0, "MpDspVtbl::size leads");
MP_STATIC_ASSERT(offsetof(MpDspVtbl, open) == 8, "MpDspVtbl::open follows the header");
/* `reset` was added after the first six modules were written. It is at the end
 * because that is the only place a vtable may grow: a host checks `size` and
 * reads no further than what it says, so a module built against the older
 * header keeps working and simply has no reset. */
MP_STATIC_ASSERT(offsetof(MpDspVtbl, describe) < offsetof(MpDspVtbl, reset),
                 "MpDspVtbl only ever grows at the end");
MP_STATIC_ASSERT(offsetof(MpDspVtbl, reset) < offsetof(MpDspVtbl, get_latency),
                 "MpDspVtbl only ever grows at the end");

MP_STATIC_ASSERT(sizeof(MpFormat) == 32, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, sample_rate) == 0, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, channels) == 4, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, channel_mask) == 8, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, sample_type) == 12, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, encoding) == 16, "MpFormat layout is ABI");
MP_STATIC_ASSERT(offsetof(MpFormat, valid_bits) == 20, "MpFormat layout is ABI");

MP_STATIC_ASSERT(offsetof(MpDeviceInfo, id) == 8, "MpDeviceInfo layout is ABI");
MP_STATIC_ASSERT(offsetof(MpDeviceInfo, name) == 264, "MpDeviceInfo layout is ABI");
MP_STATIC_ASSERT(sizeof(MpDeviceInfo) == 520, "MpDeviceInfo layout is ABI");

/* Every vtable and descriptor opens with a u32 size, so a host reading an older
 * module clamps at `size` and a newer field simply is not there. */
MP_STATIC_ASSERT(offsetof(MpHost, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpDemuxVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpCodecVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpSinkVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpDspVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpModuleDesc, size) == 0, "size must lead");
MP_STATIC_ASSERT(sizeof(MpStreamKind) == 4, "MpStreamKind is a u32 field");
MP_STATIC_ASSERT(sizeof(MpCodec) == 4, "MpCodec is a u32 field");
MP_STATIC_ASSERT(offsetof(MpStreamInfo, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpStreamInfo, format) == 24, "MpStreamInfo layout is ABI");
MP_STATIC_ASSERT(offsetof(MpPacket, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpPacket, stream) == 12, "MpPacket::stream took reserved's slot");
MP_STATIC_ASSERT(sizeof(MpPacket) == 24, "MpPacket layout is ABI");
MP_STATIC_ASSERT(offsetof(MpVideoInfo, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpVideoFrame, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpVideoVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(sizeof(MpVideoInfo) == 48, "MpVideoInfo layout is ABI");
MP_STATIC_ASSERT(offsetof(MpVideoInfo, flags) < offsetof(MpVideoInfo, timescale),
                 "MpVideoInfo only ever grows at the end");

/* v3 broke two members of MpDemuxVtbl in place and appended one after them.
 * `select_streams` and `seek` kept their slots, so the diff a reader has to
 * check is two signatures rather than a reordering; `stream_video_info` is
 * after `close`, because the end is still the only place a vtable may grow. */
MP_STATIC_ASSERT(offsetof(MpDemuxVtbl, close) < offsetof(MpDemuxVtbl, stream_video_info),
                 "MpDemuxVtbl only ever grows at the end");


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MEDIAPERCH_MODULE_H */

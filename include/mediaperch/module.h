/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MediaPerch module ABI, version 1.
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

#define MP_ABI_VERSION 1u

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
/* Decoders                                                            */
/* ------------------------------------------------------------------ */

typedef struct MpDecoder MpDecoder; /* opaque, module-owned */

typedef struct MpDecoderVtbl {
    uint32_t size;
    uint32_t reserved;

    /* MP_IO. Score 0 = cannot open, 100 = certain. `head` is the first
     * `head_bytes` of the file and may be shorter than asked for. */
    MpResult(MP_CALL *probe)(const char *path, const uint8_t *head, size_t head_bytes,
                             uint32_t *out_score);
    /* MP_IO */
    MpResult(MP_CALL *open)(const char *path, MpDecoder **out);
    MpResult(MP_CALL *get_format)(MpDecoder *d, MpFormat *out);
    MpResult(MP_CALL *get_length)(MpDecoder *d, uint64_t *out_frames);
    /* MP_IO. Fills at most `dst_bytes`, always a whole number of frames, in the
     * format `get_format` reported. Returns MP_END with *out_bytes == 0 at the
     * end of the stream. Never converts: conversion is the graph's job. */
    MpResult(MP_CALL *read)(MpDecoder *d, void *dst, size_t dst_bytes, size_t *out_bytes);
    MpResult(MP_CALL *seek)(MpDecoder *d, uint64_t frame);
    void(MP_CALL *close)(MpDecoder *d);
} MpDecoderVtbl;

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
} MpDspVtbl;

/* ------------------------------------------------------------------ */
/* The module itself                                                   */
/* ------------------------------------------------------------------ */

typedef uint32_t MpKind;
enum {
    MP_KIND_DECODER = 1u,
    MP_KIND_SINK = 2u,
    MP_KIND_DSP = 3u,   /* MpDspVtbl */
    MP_KIND_VIDEO = 4u, /* reserved */
    MP_KIND_META = 5u   /* reserved */
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

    /* MpDecoderVtbl* or MpSinkVtbl*, per `kind`. Owned by the module and valid
     * until shutdown returns. */
    const void *vtbl;
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
MP_STATIC_ASSERT(offsetof(MpDecoderVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpSinkVtbl, size) == 0, "size must lead");
MP_STATIC_ASSERT(offsetof(MpModuleDesc, size) == 0, "size must lead");

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MEDIAPERCH_MODULE_H */

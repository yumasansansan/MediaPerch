/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A module in plain C, to find out whether the ABI is one.
 *
 * **An ABI that has never been crossed from a second toolchain is an ABI that
 * does not work yet.** plan.md §2 says so and then says how to settle it
 * cheaply: about a hundred lines of C, built once, run once. This is that.
 *
 * What it is looking for, in order of how quietly each one would have failed:
 *
 *  - **A header that only compiles as C++.** `module.h` is included here by a C
 *    compiler in C mode. A stray `namespace`, a default argument, a `bool`
 *    without `<stdbool.h>`, an anonymous union used the C++ way -- any of them
 *    and this file does not build.
 *  - **Name mangling.** The host looks up `mp_module_entry` by that exact
 *    string. A C++ module gets it right by writing `extern "C"`; a C module
 *    gets it right by being C, which is the honest test of whether the host was
 *    relying on something else.
 *  - **`bool` width.** C++ `bool` and C `_Bool` are the same size on every
 *    platform this targets, and the ABI uses neither in a struct -- this is a
 *    check that it stays that way, because a `bool` in a vtable would be a
 *    field the two languages disagree about the padding of.
 *  - **Layout.** The static assertions below are the same ones the C++ header
 *    makes, evaluated by a C compiler. If a struct is laid out differently the
 *    build stops here rather than in a caller's stack.
 *  - **Calling convention.** Every function is `MP_CALL`, and the host calls
 *    them through the vtable. On 64-bit Windows there is only one convention,
 *    so this matters for the 32-bit build that does not exist yet -- which is
 *    exactly when a probe is cheap and a discovery is not.
 *
 * It is a DSP stage because that is the smallest vtable with something to say:
 * it halves what it is given, counts what it was passed, and reports through
 * `describe` whether the host kept the promises the header makes.
 *
 * Not built by default. `-DMEDIAPERCH_BUILD_ABI_PROBES=ON` turns it on.
 */

#include <mediaperch/module.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The layout claims, made again by a C compiler. The header makes these for
 * C++; a second opinion from the other language is the whole point of being
 * here. */
MP_STATIC_ASSERT(sizeof(MpFormat) == 32, "MpFormat is not what C++ thinks it is");
MP_STATIC_ASSERT(offsetof(MpFormat, sample_rate) == 0, "MpFormat.sample_rate moved");
MP_STATIC_ASSERT(offsetof(MpFormat, channels) == 4, "MpFormat.channels moved");
MP_STATIC_ASSERT(offsetof(MpFormat, channel_mask) == 8, "MpFormat.channel_mask moved");
MP_STATIC_ASSERT(offsetof(MpFormat, sample_type) == 12, "MpFormat.sample_type moved");
MP_STATIC_ASSERT(offsetof(MpFormat, encoding) == 16, "MpFormat.encoding moved");
MP_STATIC_ASSERT(offsetof(MpFormat, valid_bits) == 20, "MpFormat.valid_bits moved");
/* Two uint32s and eight function pointers, with nothing in between. */
MP_STATIC_ASSERT(sizeof(MpDspVtbl) == 8 + 8 * sizeof(void *),
                 "MpDspVtbl has grown or shrunk somewhere C cannot see");

/* An enum's underlying type is the other thing two languages can disagree
 * about. The ABI pins it by giving every enum a value that needs 32 bits. */
MP_STATIC_ASSERT(sizeof(MpResult) == 4, "MpResult is not 32 bits in C");
MP_STATIC_ASSERT(sizeof(MpSampleType) == 4, "MpSampleType is not 32 bits in C");

struct MpDsp {
    MpFormat format;
    /* What the host actually did, so `describe` can say whether it matched what
     * the header promised. */
    uint32_t configured;
    uint32_t blocks;
    uint64_t frames;
    int complaints;
};

static const MpHost *g_host = NULL;

static MpResult MP_CALL probe_open(MpDsp **out)
{
    MpDsp *d;
    if (out == NULL) {
        return MP_ERR_INVALID;
    }
    d = (MpDsp *)calloc(1, sizeof(MpDsp));
    if (d == NULL) {
        return MP_ERR_NO_MEMORY;
    }
    *out = d;
    return MP_OK;
}

static void MP_CALL probe_close(MpDsp *d)
{
    free(d);
}

static MpResult MP_CALL probe_configure(MpDsp *d, const MpFormat *in, uint32_t max_frames,
                                        MpFormat *out, uint32_t *out_max)
{
    if (d == NULL || in == NULL || out == NULL || out_max == NULL) {
        return MP_ERR_INVALID;
    }
    /* The bus is deinterleaved f64. A host that offered anything else would be
     * a host that had changed the contract without telling the modules. */
    if (in->sample_type != MP_SAMPLE_F64) {
        d->complaints++;
        return MP_ERR_FORMAT;
    }
    if (in->channels == 0 || in->sample_rate == 0 || max_frames == 0) {
        d->complaints++;
        return MP_ERR_FORMAT;
    }
    d->format = *in;
    d->configured++;
    *out = *in;
    *out_max = max_frames;
    return MP_OK;
}

static MpResult MP_CALL probe_process(MpDsp *d, const double *const *in, uint32_t in_frames,
                                      double *const *out, uint32_t out_capacity,
                                      uint32_t *out_frames)
{
    uint32_t c;
    uint32_t n;
    if (d == NULL || out_frames == NULL) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    if (in_frames == 0) {
        return MP_OK;
    }
    if (in == NULL || out == NULL) {
        d->complaints++;
        return MP_ERR_INVALID;
    }
    if (in_frames > out_capacity) {
        /* The header says the host will not do this. Counting it is how a
         * probe reports a host that does. */
        d->complaints++;
        return MP_ERR_INVALID;
    }
    for (c = 0; c < d->format.channels; ++c) {
        if (in[c] == NULL || out[c] == NULL) {
            d->complaints++;
            return MP_ERR_INVALID;
        }
        for (n = 0; n < in_frames; ++n) {
            out[c][n] = in[c][n] * 0.5;
        }
    }
    d->blocks++;
    d->frames += in_frames;
    *out_frames = in_frames;
    return MP_OK;
}

static MpResult MP_CALL probe_flush(MpDsp *d, double *const *out, uint32_t out_capacity,
                                    uint32_t *out_frames)
{
    (void)d;
    (void)out;
    (void)out_capacity;
    if (out_frames == NULL) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    return MP_OK;
}

static MpResult MP_CALL probe_set(MpDsp *d, const char *key, const char *value)
{
    if (d == NULL || key == NULL || value == NULL) {
        return MP_ERR_INVALID;
    }
    /* One setting, so that `set` is exercised rather than merely present. */
    if (strcmp(key, "reset_counts") == 0) {
        d->blocks = 0;
        d->frames = 0;
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
}

static MpResult MP_CALL probe_describe(MpDsp *d, uint32_t index, char *out,
                                       uint32_t out_bytes)
{
    if (d == NULL || out == NULL || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    switch (index) {
    case 0:
        snprintf(out, out_bytes, "language\tC\tthis module was compiled by a C compiler");
        return MP_OK;
    case 1:
        snprintf(out, out_bytes, "blocks\t%u\tprocess calls seen (read only)", d->blocks);
        return MP_OK;
    case 2:
        snprintf(out, out_bytes, "frames\t%llu\tframes seen (read only)",
                 (unsigned long long)d->frames);
        return MP_OK;
    case 3:
        /* The answer the probe exists to give. */
        snprintf(out, out_bytes, "complaints\t%d\ttimes the host broke the header's word",
                 d->complaints);
        return MP_OK;
    case 4:
        snprintf(out, out_bytes, "host\t%s\twhether a host vtable arrived at init",
                 g_host != NULL ? "yes" : "no");
        return MP_OK;
    default:
        return MP_END;
    }
}

static MpResult MP_CALL probe_reset(MpDsp *d)
{
    if (d == NULL) {
        return MP_ERR_INVALID;
    }
    return MP_OK;
}

static const MpDspVtbl g_vtbl = {
    sizeof(MpDspVtbl), 0,           &probe_open,     &probe_close, &probe_configure,
    &probe_process,    &probe_flush, &probe_set,     &probe_describe, &probe_reset,
};

static MpResult MP_CALL probe_init(const MpHost *host)
{
    g_host = host;
    return MP_OK;
}

static void MP_CALL probe_shutdown(void)
{
    g_host = NULL;
}

static const MpModuleDesc g_desc = {
    sizeof(MpModuleDesc),
    MP_ABI_VERSION,
    0,
    MP_MAKE_VERSION(0, 1, 0),
    MP_KIND_DSP,
    /* Lowest priority there is. It is a probe, not a filter anybody wants
     * chosen for them. */
    0,
    "dsp_probe_c",
    "The ABI, crossed from C (a probe: it halves what it is given)",
    &probe_init,
    &probe_shutdown,
    &g_vtbl,
};

MP_EXPORT const MpModuleDesc *MP_CALL mp_module_entry(uint32_t host_abi_version)
{
    if (host_abi_version != MP_ABI_VERSION) {
        return NULL;
    }
    return &g_desc;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// A gain, as a Path B stage.
//
// The simplest thing that is a real one, and here for two reasons rather than
// one. It is the volume control an exclusive-mode stream has nowhere else to
// put -- `ISimpleAudioVolume` and `IAudioStreamVolume` have no effect at all on
// an exclusive stream, which is a fact worth a comment beside every volume
// slider anybody writes. And it is the first implementation of `MpDspVtbl`,
// which is what turns that vtable from a design into an interface: a shape with
// one implementation is a guess, and the second one is where the wrong
// assumptions surface.
//
// ReplayGain lives here eventually, and lives here as *settings* rather than as
// another module: it is this stage with its `gain_db` set from a tag, plus the
// peak the tag also carries so the gain can be limited rather than clipped.

#include <mediaperch/module.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

const MpHost* g_host = nullptr;

} // namespace

struct MpDsp {
    double gain = 1.0;
    double gain_db = 0.0;
    /// The loudest sample seen, so a host can say what the gain did rather than
    /// guess. Reset by `configure`.
    double peak = 0.0;
    MpFormat format{};
    std::uint32_t max_frames = 0;
};

namespace {

MpResult MP_CALL dsp_open(MpDsp** out) noexcept
{
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = new (std::nothrow) MpDsp();
    return *out != nullptr ? MP_OK : MP_ERR_NO_MEMORY;
}

void MP_CALL dsp_close(MpDsp* d) noexcept
{
    delete d;
}

MpResult MP_CALL dsp_configure(MpDsp* d, const MpFormat* in, std::uint32_t max_frames,
                               MpFormat* out, std::uint32_t* out_max) noexcept
{
    if (d == nullptr || in == nullptr || out == nullptr || out_max == nullptr) {
        return MP_ERR_INVALID;
    }
    if (in->channels == 0 || in->channels > 64 || max_frames == 0) {
        return MP_ERR_FORMAT;
    }
    d->format = *in;
    d->max_frames = max_frames;
    d->peak = 0.0;

    // A gain changes nothing about the format and produces exactly what it is
    // given, which is what most stages will do and what makes a resampler the
    // interesting case rather than this one.
    *out = *in;
    *out_max = max_frames;
    return MP_OK;
}

MpResult MP_CALL dsp_process(MpDsp* d, const double* const* in, std::uint32_t in_frames,
                             double* const* out, std::uint32_t out_capacity,
                             std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    if (in_frames == 0 || in == nullptr) {
        return MP_OK;
    }
    if (in_frames > out_capacity) {
        return MP_ERR_INVALID;
    }

    for (std::uint32_t c = 0; c < d->format.channels; ++c) {
        const double* src = in[c];
        double* dst = out[c];
        for (std::uint32_t n = 0; n < in_frames; ++n) {
            const double v = src[n] * d->gain;
            dst[n] = v;
            const double magnitude = v < 0.0 ? -v : v;
            if (magnitude > d->peak) {
                d->peak = magnitude;
            }
        }
    }
    *out_frames = in_frames;
    return MP_OK;
}

MpResult MP_CALL dsp_flush(MpDsp* d, double* const* out, std::uint32_t out_capacity,
                           std::uint32_t* out_frames) noexcept
{
    (void)d;
    (void)out;
    (void)out_capacity;
    if (out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0; // no history, nothing held back
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "gain_db") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        if (end == value || !std::isfinite(db) || db > 24.0 || db < -144.0) {
            return MP_ERR_INVALID;
        }
        d->gain_db = db;
        d->gain = std::pow(10.0, db / 20.0);
        return MP_OK;
    }
    if (std::strcmp(key, "gain") == 0) {
        char* end = nullptr;
        const double linear = std::strtod(value, &end);
        if (end == value || !std::isfinite(linear) || linear < 0.0 || linear > 16.0) {
            return MP_ERR_INVALID;
        }
        d->gain = linear;
        d->gain_db = linear > 0.0 ? 20.0 * std::log10(linear) : -144.0;
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
}

MpResult MP_CALL dsp_describe(MpDsp* d, std::uint32_t index, char* out,
                              std::uint32_t out_bytes) noexcept
{
    if (d == nullptr || out == nullptr || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes, "gain_db\t%.4f\tgain in decibels, -144 to 24",
                      d->gain_db);
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes, "gain\t%.6f\tlinear gain, 0 to 16", d->gain);
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample seen (read only)",
                      d->peak);
        return MP_OK;
    default:
        return MP_END;
    }
}

const MpDspVtbl g_vtbl = {
    /* size      */ sizeof(MpDspVtbl),
    /* reserved  */ 0,
    /* open      */ &dsp_open,
    /* close     */ &dsp_close,
    /* configure */ &dsp_configure,
    /* process   */ &dsp_process,
    /* flush     */ &dsp_flush,
    /* set       */ &dsp_set,
    /* describe  */ &dsp_describe,
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

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DSP,
    /* priority    */ 100,
    /* id          */ "dsp_gain",
    /* name        */ "Gain (the volume an exclusive stream has nowhere else to put)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

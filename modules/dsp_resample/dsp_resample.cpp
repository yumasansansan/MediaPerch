// SPDX-License-Identifier: GPL-3.0-or-later
//
// The resampler, as a Path B stage.
//
// This is the stage `MpDspVtbl` was shaped for. A gain answers `configure` with
// the format it was handed; this one answers with a different sample rate, and
// everything downstream -- the chain's output format, what negotiation offers
// the device, how much room `process` needs -- follows from that answer rather
// than from anything the host assumed.
//
// **It is never inserted automatically.** A resampler that appears by itself
// whenever a device is fussy is how a bit-exact player quietly stops being one.
// It is asked for: `--dsp resample:rate=48000`.
//
// The filter itself is in resample.cpp, which is a library so that the tests can
// measure it without loading a module.

#include "resample.hpp"

#include <mediaperch/module.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {

const MpHost* g_host = nullptr;

} // namespace

struct MpDsp {
    mp::resample::Resampler engine;
    mp::resample::Design design{};
    std::string quality = "good";
    /// 0 until somebody says. A resampler with no target rate is a copy, and
    /// says so rather than guessing at one.
    std::uint32_t rate = 0;
    MpFormat format{};
    std::uint32_t max_frames = 0;
    std::string why;
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
    if (in->channels == 0 || in->channels > 64 || in->sample_rate == 0 || max_frames == 0) {
        return MP_ERR_FORMAT;
    }
    if (in->sample_type != MP_SAMPLE_F64) {
        // The bus is f64 and a stage is entitled to insist on it rather than
        // convert one silently.
        return MP_ERR_FORMAT;
    }

    const std::uint32_t target = d->rate != 0 ? d->rate : in->sample_rate;
    if (!d->engine.configure(in->sample_rate, target, in->channels, d->design, d->why)) {
        return MP_ERR_FORMAT;
    }

    d->format = *in;
    d->max_frames = max_frames;

    *out = *in;
    out->sample_rate = target;
    *out_max = d->engine.max_output(max_frames);
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
    std::uint32_t produced = 0;
    if (!d->engine.process(in, in_frames, out, out_capacity, produced)) {
        return MP_ERR_INVALID;
    }
    *out_frames = produced;
    return MP_OK;
}

MpResult MP_CALL dsp_flush(MpDsp* d, double* const* out, std::uint32_t out_capacity,
                           std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    std::uint32_t produced = 0;
    if (!d->engine.flush(out, out_capacity, produced)) {
        return MP_ERR_INVALID;
    }
    *out_frames = produced;
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "rate") == 0) {
        char* end = nullptr;
        const unsigned long rate = std::strtoul(value, &end, 10);
        // The window is the ABI's own: 1 Hz is not audio and 3 MHz is past what
        // any container can say. The device decides the rest.
        if (end == value || rate < 4000 || rate > 3'000'000) {
            return MP_ERR_INVALID;
        }
        d->rate = static_cast<std::uint32_t>(rate);
        return MP_OK;
    }
    if (std::strcmp(key, "quality") == 0) {
        if (!mp::resample::design_from_name(value, d->design)) {
            return MP_ERR_INVALID;
        }
        d->quality = value;
        return MP_OK;
    }
    if (std::strcmp(key, "attenuation") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        if (end == value || !std::isfinite(db) || db < 40.0 || db > 200.0) {
            return MP_ERR_INVALID;
        }
        d->design.attenuation_db = db;
        d->quality = "custom";
        return MP_OK;
    }
    if (std::strcmp(key, "bandwidth") == 0) {
        char* end = nullptr;
        const double fraction = std::strtod(value, &end);
        if (end == value || !std::isfinite(fraction) || fraction < 0.5 || fraction > 0.999) {
            return MP_ERR_INVALID;
        }
        d->design.bandwidth = fraction;
        d->quality = "custom";
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
        std::snprintf(out, out_bytes, "rate\t%u\ttarget sample rate; 0 leaves it alone",
                      d->rate);
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes, "quality\t%s\t%s", d->quality.c_str(),
                      mp::resample::quality_names().c_str());
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes,
                      "attenuation\t%.1f\tstopband, in dB: also the aliasing floor",
                      d->design.attenuation_db);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "bandwidth\t%.3f\tpassband, as a fraction of the lower Nyquist",
                      d->design.bandwidth);
        return MP_OK;
    case 4:
        // Read-only, and the three numbers that say what was actually built.
        std::snprintf(out, out_bytes, "ratio\t%u/%u\tup/down, reduced (read only)",
                      d->engine.up(), d->engine.down());
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes,
                      "taps\t%u\tmultiplies per output sample (read only)",
                      d->engine.identity() ? 0u : d->engine.taps_per_phase());
        return MP_OK;
    case 6:
        std::snprintf(out, out_bytes,
                      "latency\t%.1f\tinput frames, already compensated (read only)",
                      d->engine.identity() ? 0.0 : d->engine.latency_frames());
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
    /* id          */ "dsp_resample",
    /* name        */ "Resample (polyphase, Kaiser-windowed sinc, rational ratios)",
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

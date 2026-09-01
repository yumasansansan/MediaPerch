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
    mp::resample::Cascade engine;
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
    if (!d->engine.configure(in->sample_rate, target, in->channels, max_frames, d->design,
                             d->why)) {
        // The host prints `why` through the chain's own refusal, which is the
        // only place a person can be told *which* stage would not configure.
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
    if (std::strcmp(key, "design") == 0) {
        if (!mp::resample::method_from_name(value, d->design.method)) {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "window") == 0) {
        if (!mp::resample::window_from_name(value, d->design.window)) {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "phase") == 0) {
        if (!mp::resample::phase_from_name(value, d->design.phase)) {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "passband_ripple") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        // Zero means "whatever the stopband gets", which is the only thing the
        // window method can say. Anything else is a real second specification
        // and only Parks-McClellan can spend it.
        if (end == value || !std::isfinite(db) || db < 0.0 || db > 6.0) {
            return MP_ERR_INVALID;
        }
        d->design.passband_ripple_db = db;
        d->quality = "custom";
        return MP_OK;
    }
    if (std::strcmp(key, "taps") == 0) {
        char* end = nullptr;
        const unsigned long taps = std::strtoul(value, &end, 10);
        if (end == value || taps > 1u << 20) {
            return MP_ERR_INVALID;
        }
        d->design.taps = static_cast<std::uint32_t>(taps);
        return MP_OK;
    }
    if (std::strcmp(key, "max_taps") == 0) {
        char* end = nullptr;
        const unsigned long taps = std::strtoul(value, &end, 10);
        if (end == value || taps < 64 || taps > (1u << 26)) {
            return MP_ERR_INVALID;
        }
        d->design.max_taps = static_cast<std::uint32_t>(taps);
        return MP_OK;
    }
    const auto counted = [&](unsigned long low, unsigned long high,
                             std::uint32_t& target) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || parsed < low || parsed > high) {
            return false;
        }
        target = static_cast<std::uint32_t>(parsed);
        return true;
    };
    if (std::strcmp(key, "remez_max_taps") == 0) {
        return counted(65, 1u << 20, d->design.remez_max_taps) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "refine_rounds") == 0) {
        return counted(1, 10000, d->design.refine_rounds) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "refine_patience") == 0) {
        return counted(1, 1000, d->design.refine_patience) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "measure_points") == 0) {
        return counted(4096, 1u << 24, d->design.measure_points) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "cepstrum") == 0) {
        char* end = nullptr;
        const unsigned long factor = std::strtoul(value, &end, 10);
        if (end == value || factor < 2 || factor > 256) {
            return MP_ERR_INVALID;
        }
        d->design.cepstrum = static_cast<std::uint32_t>(factor);
        return MP_OK;
    }
    if (std::strcmp(key, "phase_floor") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        if (end == value || !std::isfinite(db) || db > 0.0 || db < -400.0) {
            return MP_ERR_INVALID;
        }
        d->design.phase_floor_db = db;
        return MP_OK;
    }
    if (std::strcmp(key, "stages") == 0) {
        if (std::strcmp(value, "auto") == 0) {
            d->design.stages = 0;
            return MP_OK;
        }
        char* end = nullptr;
        const unsigned long stages = std::strtoul(value, &end, 10);
        if (end == value || stages > 6) {
            return MP_ERR_INVALID;
        }
        d->design.stages = static_cast<std::uint32_t>(stages);
        return MP_OK;
    }
    if (std::strcmp(key, "verify") == 0) {
        d->design.verify = std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
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
        std::snprintf(out, out_bytes, "design\t%s\twindow, remez or refine",
                      mp::resample::method_name(d->design.method));
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes,
                      "window\t%s\tkaiser, dpss or dolph (design=window only)",
                      mp::resample::window_name(d->design.window));
        return MP_OK;
    case 14:
        std::snprintf(out, out_bytes,
                      "phase\t%s\tlinear keeps the alignment exact; minimum has no "
                      "pre-ringing and no exact alignment",
                      mp::resample::phase_name(d->design.phase));
        return MP_OK;
    case 15:
        if (d->design.stages == 0) {
            std::snprintf(out, out_bytes,
                          "stages\tauto\thow many steps the conversion may take");
        } else {
            std::snprintf(out, out_bytes,
                          "stages\t%u\thow many steps the conversion may take; auto "
                          "searches for the cheapest",
                          d->design.stages);
        }
        return MP_OK;
    case 17:
        std::snprintf(out, out_bytes,
                      "cepstrum\t%u\ttransform length as a multiple of the filter "
                      "(phase=minimum only)",
                      d->design.cepstrum);
        return MP_OK;
    case 18:
        if (d->design.phase_floor_db < 0.0) {
            std::snprintf(out, out_bytes,
                          "phase_floor\t%.1f\tdB below the peak where the logarithm "
                          "stops (phase=minimum only)",
                          d->design.phase_floor_db);
        } else {
            std::snprintf(out, out_bytes,
                          "phase_floor\tauto\tdB below the peak where the logarithm "
                          "stops; auto is 20 under the stopband");
        }
        return MP_OK;
    case 19:
        std::snprintf(out, out_bytes,
                      "remez_max_taps\t%u\tthe longest prototype design=remez will "
                      "attempt",
                      d->design.remez_max_taps);
        return MP_OK;
    case 20:
        std::snprintf(out, out_bytes,
                      "refine_rounds\t%u\tprojection rounds (design=refine only)",
                      d->design.refine_rounds);
        return MP_OK;
    case 21:
        std::snprintf(out, out_bytes,
                      "refine_patience\t%u\tfruitless rounds before stopping "
                      "(design=refine only)",
                      d->design.refine_patience);
        return MP_OK;
    case 22:
        std::snprintf(out, out_bytes,
                      "measure_points\t%u\tceiling on the transform the response is "
                      "read from",
                      d->design.measure_points);
        return MP_OK;
    case 16: {
        // The plan that was actually built, which is the only way to see what
        // `stages=auto` decided.
        std::string plan;
        for (std::size_t i = 0; i < d->engine.size(); ++i) {
            if (i != 0) {
                plan += " -> ";
            }
            plan += std::to_string(d->engine.stage(i).up()) + "/" +
                    std::to_string(d->engine.stage(i).down()) + " (" +
                    std::to_string(d->engine.stage(i).taps_per_phase()) + " taps)";
        }
        std::snprintf(out, out_bytes, "plan\t%s\tthe steps that were built (read only)",
                      plan.c_str());
        return MP_OK;
    }
    case 6:
        std::snprintf(out, out_bytes,
                      "passband_ripple\t%.4f\tdB; 0 means the same as the stopband",
                      d->design.passband_ripple_db);
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes,
                      "taps\t%u\tper phase; 0 derives it from the specification",
                      d->design.taps);
        return MP_OK;
    case 8:
        std::snprintf(out, out_bytes, "verify\t%s\trefuse a design that misses its spec",
                      d->design.verify ? "1" : "0");
        return MP_OK;
    case 9:
        // Everything past here is read-only: what was actually built, and what
        // it actually measured. A setting says what was asked for; these say
        // what came back.
        std::snprintf(out, out_bytes, "ratio\t%u/%u\tup/down, reduced (read only)",
                      d->engine.up(), d->engine.down());
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes, "multiplies\t%.0f\tper output frame (read only)",
                      d->engine.identity() ? 0.0 : d->engine.multiplies());
        return MP_OK;
    case 11:
        std::snprintf(out, out_bytes,
                      "latency\t%.2f\tinput frames left after alignment; %s (read only)",
                      d->engine.identity() ? 0.0 : d->engine.latency_frames(),
                      d->engine.aligned() ? "exact" : "minimum phase, so not exact");
        return MP_OK;
    case 12:
        std::snprintf(out, out_bytes,
                      "measured_stopband\t%.2f\tdB, off the built filter (read only)",
                      d->engine.response().stopband_db);
        return MP_OK;
    case 13:
        std::snprintf(out, out_bytes,
                      "measured_passband\t%.2e\tdB of ripple, off the built filter (read "
                      "only)",
                      d->engine.response().passband_ripple_db);
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

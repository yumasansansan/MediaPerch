// SPDX-License-Identifier: GPL-3.0-or-later
//
// Convolution with an impulse response somebody measured.
//
// The engine is [modules/convolve](../convolve/convolve.hpp) -- the same one
// the equaliser's FIR modes run on -- so what is here is the four questions a
// *file* raises that a designed filter does not: what rate it was measured at,
// how many channels it has, how long it is, and how loud. `impulse.cpp` answers
// them and this reports every answer.
//
// **The impulse is resampled to the stream's rate rather than refused.** That
// is the same decision the equaliser makes when it re-derives its biquads at
// each rate instead of transcribing them at one: the filter is not the audio,
// and a filter that does not match the audio's rate is simply the wrong filter.
// Refusing here would be refusing to design.
//
// **Nothing about the level is changed unless asked.** A room correction is
// already at the level its author meant. The gains are reported -- the response
// at DC especially, which is what decides whether everything gets louder -- and
// `normalise` is there for the impulse responses that are not corrections.

#include "impulse.hpp"

#include <convolve.hpp>
#include <mediaperch/module.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {

const MpHost* g_host = nullptr;

/// What to do about an impulse response whose gain is not one.
enum class Normalise : std::uint32_t {
    /// As measured. The right answer for a correction, which is already at the
    /// level it was computed for.
    none,
    /// So the response at zero hertz is unity: a steady signal comes out at the
    /// level it went in.
    dc,
    /// So the largest tap is one.
    peak,
    /// So an uncorrelated signal keeps its power. What a reverb wants.
    energy,
};

} // namespace

struct MpDsp {
    mp::convolve::Convolver convolver;
    mp::impulse::Response loaded;   ///< as it came off disk
    mp::impulse::Gains gains{};     ///< of what was actually built
    std::string path;
    Normalise normalise = Normalise::none;
    double gain_db = 0.0;
    std::uint32_t partition = 0;
    std::uint32_t max_taps = 0;
    bool resample = true;
    std::uint32_t built_rate = 0;
    std::size_t built_taps = 0;
    double peak = 0.0;
    MpFormat format{};
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
        return MP_ERR_FORMAT;
    }
    if (d->loaded.empty()) {
        d->why = "no impulse response: --dsp convolve:file=<path>";
        return MP_ERR_FORMAT;
    }

    // Work on a copy: the file is read once and the same file has to survive
    // being configured for a second rate when the next track has one.
    mp::impulse::Response response = d->loaded;

    if (response.sample_rate != in->sample_rate) {
        if (!d->resample) {
            d->why = "that response was measured at " +
                     std::to_string(response.sample_rate) + " Hz and the stream is at " +
                     std::to_string(in->sample_rate) +
                     "; set resample=1 or use a response at the right rate";
            return MP_ERR_FORMAT;
        }
        if (!mp::impulse::resample_to(response, in->sample_rate, d->why)) {
            return MP_ERR_FORMAT;
        }
    }
    if (!mp::impulse::fit_channels(response, in->channels, d->why)) {
        return MP_ERR_FORMAT;
    }
    mp::impulse::truncate(response, d->max_taps);

    const mp::impulse::Gains before = mp::impulse::measure(response);
    double factor = std::pow(10.0, d->gain_db / 20.0);
    switch (d->normalise) {
    case Normalise::dc:
        if (before.dc != 0.0) {
            factor /= std::abs(before.dc);
        }
        break;
    case Normalise::peak:
        if (before.peak > 0.0) {
            factor /= before.peak;
        }
        break;
    case Normalise::energy:
        if (before.energy > 0.0) {
            factor /= before.energy;
        }
        break;
    case Normalise::none:
        break;
    }
    if (factor != 1.0) {
        mp::impulse::scale(response, factor);
    }
    d->gains = mp::impulse::measure(response);
    d->built_rate = response.sample_rate;
    d->built_taps = response.frames();

    const std::uint32_t partition =
        d->partition != 0 ? d->partition
                          : std::min<std::uint32_t>(4096, std::max(64u, max_frames));
    if (!d->convolver.configure(response.channels, partition, d->why)) {
        return MP_ERR_FORMAT;
    }

    d->format = *in;
    d->peak = 0.0;
    *out = *in;
    *out_max = d->convolver.max_output(max_frames);
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

    std::uint32_t made = 0;
    d->convolver.process(in, in_frames, out, out_capacity, made);
    for (std::uint32_t c = 0; c < d->format.channels; ++c) {
        const double* dst = out[c];
        for (std::uint32_t n = 0; n < made; ++n) {
            const double magnitude = dst[n] < 0.0 ? -dst[n] : dst[n];
            if (magnitude > d->peak) {
                d->peak = magnitude;
            }
        }
    }
    *out_frames = made;
    return MP_OK;
}

MpResult MP_CALL dsp_flush(MpDsp* d, double* const* out, std::uint32_t out_capacity,
                           std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    // An impulse response has a length, so its tail has one too: exactly as
    // many frames as it has taps, and not one more.
    std::uint32_t made = 0;
    d->convolver.flush(out, out_capacity, made);
    *out_frames = made;
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "file") == 0) {
        mp::impulse::Response response;
        if (!mp::impulse::load(value, response, d->why)) {
            return MP_ERR_INVALID;
        }
        d->loaded = std::move(response);
        d->path = value;
        return MP_OK;
    }
    if (std::strcmp(key, "normalise") == 0 || std::strcmp(key, "normalize") == 0) {
        if (std::strcmp(value, "none") == 0) {
            d->normalise = Normalise::none;
        } else if (std::strcmp(value, "dc") == 0) {
            d->normalise = Normalise::dc;
        } else if (std::strcmp(value, "peak") == 0) {
            d->normalise = Normalise::peak;
        } else if (std::strcmp(value, "energy") == 0) {
            d->normalise = Normalise::energy;
        } else {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "gain_db") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        if (end == value || !std::isfinite(db) || db < -60.0 || db > 30.0) {
            return MP_ERR_INVALID;
        }
        d->gain_db = db;
        return MP_OK;
    }
    if (std::strcmp(key, "partition") == 0) {
        char* end = nullptr;
        const unsigned long partition = std::strtoul(value, &end, 10);
        if (end == value || partition > (1u << 20)) {
            return MP_ERR_INVALID;
        }
        d->partition = static_cast<std::uint32_t>(partition);
        return MP_OK;
    }
    if (std::strcmp(key, "max_taps") == 0) {
        char* end = nullptr;
        const unsigned long taps = std::strtoul(value, &end, 10);
        if (end == value || taps > (1u << 24)) {
            return MP_ERR_INVALID;
        }
        d->max_taps = static_cast<std::uint32_t>(taps);
        return MP_OK;
    }
    if (std::strcmp(key, "resample") == 0) {
        d->resample = std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
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
    const char* normalise = d->normalise == Normalise::none
                                ? "none"
                                : (d->normalise == Normalise::dc
                                       ? "dc"
                                       : (d->normalise == Normalise::peak ? "peak"
                                                                          : "energy"));
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes, "file\t%s\tthe impulse response to convolve with",
                      d->path.empty() ? "none" : d->path.c_str());
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes,
                      "normalise\t%s\tnone (as measured), dc, peak or energy",
                      normalise);
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "gain_db\t%+.2f\tdB applied on top of that",
                      d->gain_db);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "max_taps\t%u\ttruncate past this, with a fade; 0 keeps it all",
                      d->max_taps);
        return MP_OK;
    case 4:
        std::snprintf(out, out_bytes,
                      "partition\t%u\tconvolution partition; 0 follows the block size",
                      d->partition);
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes,
                      "resample\t%s\tconvert the response to the stream's rate",
                      d->resample ? "1" : "0");
        return MP_OK;
    case 6:
        std::snprintf(out, out_bytes,
                      "measured_at\t%u\tHz, as the file was recorded (read only)",
                      d->loaded.sample_rate);
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes,
                      "built\t%zu taps at %u Hz\twhat is actually convolving (read only)",
                      d->built_taps, d->built_rate);
        return MP_OK;
    case 8:
        // The number that decides whether everything gets louder. Reported
        // whether or not anything was done about it.
        std::snprintf(out, out_bytes,
                      "gain_at_dc\t%+.2f\tdB the response has at zero hertz (read only)",
                      d->gains.dc != 0.0 ? 20.0 * std::log10(std::abs(d->gains.dc))
                                         : -400.0);
        return MP_OK;
    case 9:
        std::snprintf(out, out_bytes,
                      "gain_energy\t%+.2f\tdB it has for uncorrelated signal (read only)",
                      d->gains.energy > 0.0 ? 20.0 * std::log10(d->gains.energy) : -400.0);
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample produced (read only)",
                      d->peak);
        return MP_OK;
    case 11:
        std::snprintf(out, out_bytes,
                      "cost\t%.0f\tmultiplies per output frame (read only)",
                      d->convolver.multiplies());
        return MP_OK;
    default:
        return MP_END;
    }
}

MpResult MP_CALL dsp_reset(MpDsp* d) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    d->convolver.reset();
    d->peak = 0.0;
    return MP_OK;
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
    /* reset     */ &dsp_reset,
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
    /* id          */ "dsp_convolve",
    /* name        */ "Convolve (an impulse response somebody measured)",
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

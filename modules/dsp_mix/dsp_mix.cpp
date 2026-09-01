// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel mixer, as a Path B stage.
//
// The third and last geometry a stage can change: the converter changes the
// sample type, `dsp_resample` changes the rate, and this changes the channel
// count. With it, the only thing left that can make a device refuse a file is a
// device that refuses everything.
//
// **Order matters, and matters for one reason.** Every stage in this chain is
// linear, so a gain, a resample and a mix commute exactly -- put them in any
// order and the samples that come out are the same to within the last bit of a
// double. What does not commute is the *cost*: resampling six channels and then
// throwing four away is three times the work of throwing them away first.
// `--dsp mix:channels=2 --dsp resample:rate=48000` is the cheap order for a
// downmix and the reverse is the cheap order for an upmix. Nothing here
// reorders anything; the chain runs in the order it was given, and the report
// prints that order.

#include "mix.hpp"

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
    mp::mix::Recipe recipe{};
    mp::mix::Matrix matrix{};
    /// 0 until somebody says. A mixer with no target is a copy.
    std::uint32_t channels = 0;
    std::uint32_t mask = 0;
    std::string explicit_matrix;
    /// The loudest sample it produced, so a downmix that clipped can be seen
    /// rather than guessed at.
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
    if (in->channels == 0 || in->channels > 64 || max_frames == 0) {
        return MP_ERR_FORMAT;
    }
    if (in->sample_type != MP_SAMPLE_F64) {
        return MP_ERR_FORMAT; // the bus is f64 and a stage may insist on it
    }

    const std::uint32_t target = d->channels != 0 ? d->channels : in->channels;
    std::uint32_t target_mask = d->mask;
    if (target_mask == 0) {
        target_mask = target == in->channels ? in->channel_mask
                                             : mp::mix::conventional_mask(target);
    }

    const bool ok =
        d->explicit_matrix.empty()
            ? mp::mix::build(in->channels, in->channel_mask, target, target_mask,
                             d->recipe, d->matrix, d->why)
            : mp::mix::parse_matrix(d->explicit_matrix, in->channels, target, d->matrix,
                                    d->why);
    if (!ok) {
        return MP_ERR_FORMAT;
    }

    d->format = *in;
    d->peak = 0.0;

    *out = *in;
    out->channels = target;
    out->channel_mask = target_mask;
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

    const std::uint32_t inputs = d->matrix.inputs;
    const std::uint32_t outputs = d->matrix.outputs;
    const double scale = d->matrix.scale;

    for (std::uint32_t o = 0; o < outputs; ++o) {
        double* dst = out[o];
        for (std::uint32_t n = 0; n < in_frames; ++n) {
            dst[n] = 0.0;
        }
        for (std::uint32_t i = 0; i < inputs; ++i) {
            const double coefficient = d->matrix.at(o, i) * scale;
            if (coefficient == 0.0) {
                continue; // most of a downmix matrix is zeros
            }
            const double* src = in[i];
            for (std::uint32_t n = 0; n < in_frames; ++n) {
                dst[n] += coefficient * src[n];
            }
        }
        for (std::uint32_t n = 0; n < in_frames; ++n) {
            const double magnitude = dst[n] < 0.0 ? -dst[n] : dst[n];
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
    *out_frames = 0; // a matrix has no memory
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto number = [&](double low, double high, double& target) {
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end == value || !std::isfinite(parsed) || parsed < low || parsed > high) {
            return false;
        }
        target = parsed;
        return true;
    };

    if (std::strcmp(key, "channels") == 0) {
        char* end = nullptr;
        const unsigned long channels = std::strtoul(value, &end, 10);
        if (end == value || channels == 0 || channels > 64) {
            return MP_ERR_INVALID;
        }
        d->channels = static_cast<std::uint32_t>(channels);
        return MP_OK;
    }
    if (std::strcmp(key, "mask") == 0) {
        char* end = nullptr;
        const unsigned long mask = std::strtoul(value, &end, 0); // 0x… is welcome
        if (end == value || mask > 0xFFFFFFFFul) {
            return MP_ERR_INVALID;
        }
        d->mask = static_cast<std::uint32_t>(mask);
        return MP_OK;
    }
    if (std::strcmp(key, "centre") == 0 || std::strcmp(key, "center") == 0) {
        return number(-400.0, 12.0, d->recipe.centre_db) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "surround") == 0) {
        return number(-400.0, 12.0, d->recipe.surround_db) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "lfe") == 0) {
        return number(-1000.0, 12.0, d->recipe.lfe_db) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "normalise") == 0 || std::strcmp(key, "normalize") == 0) {
        return mp::mix::normalise_from_name(value, d->recipe.normalise) ? MP_OK
                                                                       : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "synthesise") == 0 || std::strcmp(key, "synthesize") == 0) {
        d->recipe.synthesise =
            std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
        return MP_OK;
    }
    if (std::strcmp(key, "matrix") == 0) {
        d->explicit_matrix = std::strcmp(value, "auto") == 0 ? "" : value;
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
        std::snprintf(out, out_bytes, "channels\t%u\ttarget channel count; 0 leaves it",
                      d->channels);
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes,
                      "mask\t0x%x\ttarget speaker mask; 0 takes the conventional one",
                      d->mask);
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes,
                      "centre\t%.2f\tdB of the centre channel into each front speaker",
                      d->recipe.centre_db);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "surround\t%.2f\tdB of each surround into the front on its side",
                      d->recipe.surround_db);
        return MP_OK;
    case 4:
        if (d->recipe.lfe_db <= -400.0) {
            std::snprintf(out, out_bytes,
                          "lfe\toff\tdB of the effects channel into the front pair; "
                          "dropped by default");
        } else {
            std::snprintf(out, out_bytes,
                          "lfe\t%.2f\tdB of the effects channel into the front pair",
                          d->recipe.lfe_db);
        }
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes,
                      "normalise\t%s\tnone, peak (cannot clip) or energy (keeps loudness)",
                      mp::mix::normalise_name(d->recipe.normalise));
        return MP_OK;
    case 6:
        std::snprintf(out, out_bytes,
                      "synthesise\t%s\tderive a channel nothing feeds, instead of silence",
                      d->recipe.synthesise ? "1" : "0");
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes, "matrix\t%s\texplicit coefficients, or auto",
                      d->explicit_matrix.empty() ? "auto" : d->explicit_matrix.c_str());
        return MP_OK;
    case 8: {
        // What was actually built. A downmix is a decision and this is the
        // decision, in the same words the setting takes.
        const std::string text = d->matrix.text();
        std::snprintf(out, out_bytes, "built\t%s\tthe matrix in use (read only)",
                      text.c_str());
        return MP_OK;
    }
    case 9:
        std::snprintf(out, out_bytes,
                      "level\t%.2f\tdB the matrix was scaled by to normalise (read only)",
                      d->matrix.scale > 0.0 ? 20.0 * std::log10(d->matrix.scale) : -400.0);
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample produced (read only)",
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
    /* id          */ "dsp_mix",
    /* name        */ "Mix (the channel matrix, and what a downmix decides)",
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

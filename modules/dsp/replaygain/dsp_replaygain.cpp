// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReplayGain, as a Path B stage: a meter that always runs and a gain that only
// runs when it has been given a number.
//
// **Measuring and applying are two different operations and they cannot happen
// in the same pass.** The integrated loudness of a track is not known until the
// track has finished, so a stage that tried to normalise what it was hearing
// would be a compressor. What this does instead is honest about the order:
// while it plays it *measures*, and reports what it measured; the gain it
// applies is one somebody gave it, from a previous scan or from a tag.
//
// `mediaperch-probe loudness --file X` is the scan, and it is the same meter.

#include "loudness.hpp"

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
    mp::loudness::Meter meter;
    /// The gain to apply, in dB, or nothing. Separate from what the meter
    /// suggests, because applying what you are still measuring is a different
    /// device with a different name.
    double gain_db = 0.0;
    bool has_gain = false;
    double target_lufs = mp::loudness::k_reference_lufs;
    double preamp_db = 0.0;
    /// The track's peak, when it is known from a scan or a tag. With it, the
    /// gain can be limited so the result does not clip; without it, it cannot.
    double known_peak = 0.0;
    bool prevent_clipping = true;
    double applied = 1.0;
    MpFormat format{};
    std::string why;
};

namespace {

double applied_gain(const MpDsp& d)
{
    if (!d.has_gain) {
        return 1.0;
    }
    double db = d.gain_db + d.preamp_db;
    if (d.prevent_clipping && d.known_peak > 0.0) {
        // Never past the point where the loudest sample reaches full scale.
        // A tag that asks for +6 dB on a track that already peaks at -1 is
        // asking for clipping, and quietly obliging is how a normaliser gets a
        // reputation.
        const double ceiling = -20.0 * std::log10(d.known_peak);
        db = std::min(db, ceiling);
    }
    return std::pow(10.0, db / 20.0);
}

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
    if (!d->meter.configure(in->sample_rate, in->channels, in->channel_mask, d->why)) {
        return MP_ERR_FORMAT;
    }
    d->format = *in;
    d->applied = applied_gain(*d);
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

    // Measured before the gain, so what is reported is the track's loudness and
    // not this stage's opinion of it.
    d->meter.add(in, in_frames);

    const double gain = d->applied;
    for (std::uint32_t c = 0; c < d->format.channels; ++c) {
        const double* src = in[c];
        double* dst = out[c];
        for (std::uint32_t n = 0; n < in_frames; ++n) {
            dst[n] = src[n] * gain;
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
    *out_frames = 0;
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

    if (std::strcmp(key, "gain_db") == 0) {
        if (!number(-60.0, 60.0, d->gain_db)) {
            return MP_ERR_INVALID;
        }
        d->has_gain = true;
        d->applied = applied_gain(*d);
        return MP_OK;
    }
    if (std::strcmp(key, "target") == 0) {
        return number(-40.0, 0.0, d->target_lufs) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "preamp") == 0) {
        if (!number(-20.0, 20.0, d->preamp_db)) {
            return MP_ERR_INVALID;
        }
        d->applied = applied_gain(*d);
        return MP_OK;
    }
    if (std::strcmp(key, "peak") == 0) {
        if (!number(0.0, 64.0, d->known_peak)) {
            return MP_ERR_INVALID;
        }
        d->applied = applied_gain(*d);
        return MP_OK;
    }
    if (std::strcmp(key, "prevent_clipping") == 0) {
        d->prevent_clipping =
            std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
        d->applied = applied_gain(*d);
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
    const double measured = d->meter.integrated_lufs();
    const bool heard = measured > mp::loudness::Meter::silence() / 2.0;

    switch (index) {
    case 0:
        if (d->has_gain) {
            std::snprintf(out, out_bytes, "gain_db\t%+.2f\tdB to apply, from a scan or a tag",
                          d->gain_db);
        } else {
            std::snprintf(out, out_bytes,
                          "gain_db\tnone\tdB to apply, from a scan or a tag; nothing "
                          "is applied without one");
        }
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes, "target\t%.1f\tLUFS the gain aims at (-18 is "
                                      "ReplayGain 2.0's own)",
                      d->target_lufs);
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "preamp\t%+.2f\tdB added to whatever gain is applied",
                      d->preamp_db);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "peak\t%.6f\tthe track's known peak, so the gain can be limited",
                      d->known_peak);
        return MP_OK;
    case 4:
        std::snprintf(out, out_bytes,
                      "prevent_clipping\t%s\tnever apply more than the known peak allows",
                      d->prevent_clipping ? "1" : "0");
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes, "applied\t%+.2f\tdB actually applied (read only)",
                      d->applied > 0.0 ? 20.0 * std::log10(d->applied) : -400.0);
        return MP_OK;
    case 6:
        // What the meter heard, so far. On a whole track this is the scan.
        if (heard) {
            std::snprintf(out, out_bytes,
                          "loudness\t%.2f\tLUFS integrated so far (read only)", measured);
        } else {
            std::snprintf(out, out_bytes,
                          "loudness\tsilence\tnothing above the absolute gate yet "
                          "(read only)");
        }
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes,
                      "measured_peak\t%.2f\tdBFS, sample peak not true peak (read only)",
                      d->meter.sample_peak_db());
        return MP_OK;
    case 8:
        if (heard) {
            std::snprintf(out, out_bytes,
                          "suggested\t%+.2f\tdB this would need to reach the target "
                          "(read only)",
                          d->meter.replay_gain_db(d->target_lufs));
        } else {
            std::snprintf(out, out_bytes,
                          "suggested\t--\tnot enough audio to say yet (read only)");
        }
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
    // The meter is reset too, and that is the honest choice: after a seek it
    // would otherwise be reporting the loudness of a track nobody heard all of.
    d->meter.reset();
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
    /* id          */ "dsp_replaygain",
    /* name        */ "ReplayGain (BS.1770 loudness, measured here rather than read)",
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

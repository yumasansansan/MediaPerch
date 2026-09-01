// SPDX-License-Identifier: GPL-3.0-or-later
//
// The equaliser: a cascade of second-order sections, anywhere on the axis.
//
// **The frequency axis is continuous and stays continuous.** A band is a
// frequency in hertz, a gain in decibels and a Q -- not a slider in a bank of
// thirty-one -- so there is nothing to interpolate between and nothing snaps to
// a preset. `curve` reports the composite response at whatever frequencies are
// asked for, computed from the coefficients rather than measured, which is what
// a display would draw and what the tests check.
//
// It changes no geometry. The rate, the channel count and the sample type all
// come out as they went in, which is why it can sit anywhere in the chain --
// though `headroom` is worth reading before it sits after something loud.

#include "biquad.hpp"

#include <mediaperch/module.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

} // namespace

struct MpDsp {
    std::vector<mp::biquad::Band> bands;
    mp::biquad::Cascade cascade;
    /// Where `curve` is sampled, and how many points it reports.
    double curve_low_hz = 20.0;
    double curve_high_hz = 20000.0;
    std::uint32_t curve_points = 9;
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
    // A band above Nyquist is refused here rather than warped down to fit,
    // which is the sort of accommodation that turns a 24 kHz shelf into an
    // audible one.
    if (!d->cascade.configure(d->bands, in->sample_rate, in->channels, d->why)) {
        return MP_ERR_FORMAT;
    }

    d->format = *in;
    d->peak = 0.0;
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

    d->cascade.process(in, in_frames, out);
    for (std::uint32_t c = 0; c < d->format.channels; ++c) {
        const double* dst = out[c];
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
    (void)out;
    (void)out_capacity;
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    // A recursive filter's tail never quite reaches zero, so there is no honest
    // number of frames to drain. What it holds is below the quantiser's last
    // bit within a few hundred samples of the end, and inventing a tail longer
    // than the file would be worse than stopping.
    *out_frames = 0;
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "bands") == 0) {
        return mp::biquad::parse_bands(value, d->bands, d->why) ? MP_OK : MP_ERR_INVALID;
    }
    if (std::strcmp(key, "band") == 0) {
        // Appends, so a shell can build a chain one band at a time and a
        // settings tree can hold them as a list rather than as a string.
        mp::biquad::Band band;
        if (!mp::biquad::parse_band(value, band, d->why)) {
            return MP_ERR_INVALID;
        }
        d->bands.push_back(band);
        return MP_OK;
    }
    if (std::strcmp(key, "clear") == 0) {
        d->bands.clear();
        return MP_OK;
    }
    if (std::strcmp(key, "curve") == 0) {
        // `low:high:points`, which is what a display asks for.
        double low = d->curve_low_hz;
        double high = d->curve_high_hz;
        long points = d->curve_points;
        if (std::sscanf(value, "%lf:%lf:%ld", &low, &high, &points) < 1) {
            return MP_ERR_INVALID;
        }
        if (low <= 0.0 || high <= low || points < 2 || points > 4096) {
            return MP_ERR_INVALID;
        }
        d->curve_low_hz = low;
        d->curve_high_hz = high;
        d->curve_points = static_cast<std::uint32_t>(points);
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
    case 0: {
        const std::string text = mp::biquad::bands_text(d->bands);
        std::snprintf(out, out_bytes, "bands\t%s\tkind:hz:dB:Q, semicolons between; %s",
                      text.c_str(), mp::biquad::kind_names().c_str());
        return MP_OK;
    }
    case 1:
        std::snprintf(out, out_bytes,
                      "band\t(append)\tone more band, in the same words as above");
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "curve\t%g:%g:%u\twhere the reported curve is "
                                      "sampled: low:high:points",
                      d->curve_low_hz, d->curve_high_hz, d->curve_points);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "sections\t%zu\tsecond-order sections in use (read only)",
                      d->cascade.sections());
        return MP_OK;
    case 4:
        // The one number somebody needs before putting an equaliser in front of
        // a quantiser: how much louder the loudest frequency got.
        std::snprintf(out, out_bytes,
                      "headroom\t%+.2f\tdB the loudest frequency gains (read only)",
                      d->cascade.sections() == 0 ? 0.0 : d->cascade.peak_gain_db());
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample produced (read only)",
                      d->peak);
        return MP_OK;
    case 6: {
        // The curve itself, computed from the coefficients rather than
        // measured. The same function a display would draw with.
        std::string text;
        char point[48];
        for (std::uint32_t i = 0; i < d->curve_points; ++i) {
            const double t = static_cast<double>(i) / (d->curve_points - 1);
            const double hz = d->curve_low_hz *
                              std::pow(d->curve_high_hz / d->curve_low_hz, t);
            std::snprintf(point, sizeof(point), "%s%.0f=%+.2f", i == 0 ? "" : " ", hz,
                          d->cascade.magnitude_db(hz));
            text += point;
        }
        std::snprintf(out, out_bytes, "response\t%s\tdB at those frequencies (read only)",
                      text.c_str());
        return MP_OK;
    }
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
    /* id          */ "dsp_eq",
    /* name        */ "Equaliser (biquad cascade, anywhere on the axis)",
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

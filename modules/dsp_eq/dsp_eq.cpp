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

#include "autoeq.hpp"
#include "biquad.hpp"

#include <convolve.hpp>
#include <mediaperch/module.h>
#include <transform.hpp>

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

/// How the same response is realised.
enum class Mode : std::uint32_t {
    /// The cascade itself. No latency, no pre-ringing, and the phase a
    /// minimum-phase filter has because that is what a biquad cascade is.
    iir,
    /// The same magnitude as an FIR with symmetric taps: every frequency
    /// delayed by the same amount, at the cost of half the filter of delay and
    /// the pre-ringing that symmetry implies.
    linear,
    /// The same magnitude with its energy at the front. No pre-ringing and
    /// almost no delay -- the FIR route to what the cascade already does, worth
    /// having because it controls the magnitude exactly where the cascade only
    /// approximates a target.
    minimum,
};

struct MpDsp {
    std::vector<mp::biquad::Band> bands;
    mp::biquad::Cascade cascade;
    mp::convolve::Convolver convolver;
    mp::autoeq::Profile profile;
    Mode mode = Mode::iir;
    /// FIR modes only.
    std::uint32_t taps = 4096;
    std::uint32_t partition = 0;
    double preamp_db = 0.0;
    double preamp = 1.0;
    std::vector<double> fir;
    /// Where `curve` is sampled, and how many points it reports.
    double curve_low_hz = 20.0;
    double curve_high_hz = 20000.0;
    std::uint32_t curve_points = 9;
    double peak = 0.0;
    MpFormat format{};
    std::string why;
};

namespace {

/// The response the filter is meant to have, in dB, at any frequency: the
/// bands, plus a loaded curve, plus the preamp. One function, so the three
/// modes cannot disagree about what they are realising.
double target_db(const MpDsp& d, double hz)
{
    return d.cascade.magnitude_db(hz) + mp::autoeq::curve_db(d.profile.curve, hz) +
           d.preamp_db;
}

/// The FIR that has that response.
///
/// Sampled on a grid, transformed back, truncated and windowed -- the window
/// method again, and for the same reason it is the resampler's default: it is
/// closed-form and it cannot fail. The Kaiser here is fixed at 90 dB of
/// sidelobe because what it is tapering is a smooth curve rather than a brick
/// wall, and a taper that is not the limiting factor does not need a setting.
std::vector<double> design_fir(const MpDsp& d, double rate, std::uint32_t taps,
                               bool minimum)
{
    const std::size_t points =
        mp::transform::next_power_of_two(std::max<std::size_t>(taps * 8, 4096));
    std::vector<std::complex<double>> spectrum(points, {0.0, 0.0});
    for (std::size_t k = 0; k <= points / 2; ++k) {
        const double hz = static_cast<double>(k) * rate / static_cast<double>(points);
        const double magnitude = std::pow(10.0, target_db(d, hz) / 20.0);
        spectrum[k] = {magnitude, 0.0};
        if (k != 0 && k != points / 2) {
            spectrum[points - k] = {magnitude, 0.0}; // real and even: zero phase
        }
    }
    mp::transform::fft(spectrum, true);

    // The zero-phase impulse is centred on zero and wraps; taking `taps` of it
    // centred on the middle is the truncation, and the window is what stops the
    // truncation ringing.
    std::vector<double> h(taps, 0.0);
    const std::size_t centre = taps / 2;
    const double beta = 0.1102 * (90.0 - 8.7);
    const double half = static_cast<double>(taps - 1) / 2.0;
    // A small Bessel series, the same one the resampler's window uses.
    const auto i0 = [](double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int i = 1; i < 64; ++i) {
            term *= (x / (2.0 * i)) * (x / (2.0 * i));
            sum += term;
            if (term < sum * 1e-18) {
                break;
            }
        }
        return sum;
    };
    const double denominator = i0(beta);
    for (std::size_t n = 0; n < taps; ++n) {
        const std::size_t from = (n + points - centre) % points;
        const double ratio = (static_cast<double>(n) - half) / half;
        const double window =
            i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / denominator;
        h[n] = spectrum[from].real() * window;
    }
    if (minimum) {
        // 20 dB under the deepest cut the curve asks for is far enough to be a
        // null and near enough to still be a number.
        mp::transform::to_minimum_phase(h, -120.0, 32);
    }
    return h;
}

/// The loudest the target gets anywhere on the axis, in dB. Read off the
/// target rather than the cascade, so a loaded curve counts for exactly as much
/// as a band does.
double target_peak_db(const MpDsp& d)
{
    double worst = target_db(d, 1.0);
    for (std::uint32_t i = 0; i < 4096; ++i) {
        // Logarithmic, 1 Hz to 100 kHz: that is where the bands are, and a
        // linear sweep would spend most of its points above 10 kHz.
        const double hz = std::pow(10.0, static_cast<double>(i) / 4095.0 * 5.0);
        worst = worst > target_db(d, hz) ? worst : target_db(d, hz);
    }
    return worst;
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
    // A band above Nyquist is refused here rather than warped down to fit,
    // which is the sort of accommodation that turns a 24 kHz shelf into an
    // audible one.
    if (!d->cascade.configure(d->bands, in->sample_rate, in->channels, d->why)) {
        return MP_ERR_FORMAT;
    }
    d->preamp = std::pow(10.0, d->preamp_db / 20.0);

    if (d->mode == Mode::iir && !d->profile.curve.empty()) {
        // A curve is a target, not a cascade. Fitting biquads to it is a real
        // optimisation problem and pretending to have solved it would be worse
        // than saying so.
        d->why = "a graphic curve is a target rather than a cascade; it needs "
                 "mode=linear or mode=minimum";
        return MP_ERR_FORMAT;
    }

    d->format = *in;
    d->peak = 0.0;
    *out = *in;

    if (d->mode == Mode::iir) {
        d->fir.clear();
        *out_max = max_frames;
        return MP_OK;
    }

    d->fir = design_fir(*d, in->sample_rate, d->taps, d->mode == Mode::minimum);
    const std::uint32_t partition =
        d->partition != 0 ? d->partition
                          : std::min<std::uint32_t>(4096, std::max(64u, max_frames));
    if (!d->convolver.configure(d->fir, in->channels, partition, d->why)) {
        return MP_ERR_FORMAT;
    }
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
    if (d->mode == Mode::iir && in_frames > out_capacity) {
        return MP_ERR_INVALID;
    }

    std::uint32_t made = 0;
    if (d->mode == Mode::iir) {
        d->cascade.process(in, in_frames, out);
        if (d->preamp != 1.0) {
            for (std::uint32_t c = 0; c < d->format.channels; ++c) {
                double* dst = out[c];
                for (std::uint32_t n = 0; n < in_frames; ++n) {
                    dst[n] *= d->preamp;
                }
            }
        }
        made = in_frames;
    } else {
        d->convolver.process(in, in_frames, out, out_capacity, made);
    }

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
    (void)out;
    (void)out_capacity;
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    if (d->mode != Mode::iir) {
        // An FIR has a tail of exactly its own length, so there is a number
        // here rather than a judgement.
        std::uint32_t made = 0;
        d->convolver.flush(out, out_capacity, made);
        *out_frames = made;
        return MP_OK;
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
    if (std::strcmp(key, "mode") == 0) {
        if (std::strcmp(value, "iir") == 0) {
            d->mode = Mode::iir;
        } else if (std::strcmp(value, "linear") == 0) {
            d->mode = Mode::linear;
        } else if (std::strcmp(value, "minimum") == 0 || std::strcmp(value, "min") == 0) {
            d->mode = Mode::minimum;
        } else {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "taps") == 0) {
        char* end = nullptr;
        const unsigned long taps = std::strtoul(value, &end, 10);
        if (end == value || taps < 16 || taps > (1u << 20)) {
            return MP_ERR_INVALID;
        }
        d->taps = static_cast<std::uint32_t>(taps);
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
    if (std::strcmp(key, "preamp") == 0) {
        char* end = nullptr;
        const double db = std::strtod(value, &end);
        if (end == value || !std::isfinite(db) || db < -40.0 || db > 20.0) {
            return MP_ERR_INVALID;
        }
        d->preamp_db = db;
        return MP_OK;
    }
    if (std::strcmp(key, "preset") == 0) {
        // AutoEq, or anything else in Equalizer APO's two formats.
        mp::autoeq::Profile profile;
        if (!mp::autoeq::load(value, profile, d->why)) {
            return MP_ERR_INVALID;
        }
        d->profile = profile;
        d->preamp_db = profile.preamp_db;
        // A parametric profile *is* the bands; a graphic one is a target that
        // the bands are then added to.
        if (!profile.bands.empty()) {
            d->bands = profile.bands;
        }
        if (!profile.curve.empty() && d->mode == Mode::iir) {
            d->mode = Mode::minimum;
        }
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
                      d->cascade.sections() == 0 && d->profile.curve.empty()
                          ? 0.0
                          : target_peak_db(*d));
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample produced (read only)",
                      d->peak);
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes,
                      "mode\t%s\tiir (no latency), linear (no phase shift) or minimum "
                      "(no pre-ringing)",
                      d->mode == Mode::iir ? "iir"
                                           : (d->mode == Mode::linear ? "linear"
                                                                      : "minimum"));
        return MP_OK;
    case 8:
        std::snprintf(out, out_bytes, "taps\t%u\tFIR length (mode=linear or minimum)",
                      d->taps);
        return MP_OK;
    case 9:
        std::snprintf(out, out_bytes,
                      "partition\t%u\tconvolution partition; 0 follows the block size",
                      d->partition);
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes, "preamp\t%+.2f\tdB applied with the curve",
                      d->preamp_db);
        return MP_OK;
    case 11:
        std::snprintf(out, out_bytes,
                      "preset\t%s\tan AutoEq or Equalizer APO file",
                      d->profile.kind.empty() ? "none" : d->profile.kind.c_str());
        return MP_OK;
    case 12:
        if (d->mode == Mode::iir) {
            std::snprintf(out, out_bytes,
                          "latency\t0\tframes; a cascade delays nothing (read only)");
        } else {
            // Half the filter for linear phase, and the block granularity on
            // top. Said rather than implied, because a player that shifts its
            // own audio should say by how much.
            std::snprintf(out, out_bytes,
                          "latency\t%u\tframes of delay this mode adds (read only)",
                          d->mode == Mode::linear ? d->taps / 2 : 0u);
        }
        return MP_OK;
    case 13:
        std::snprintf(out, out_bytes,
                      "cost\t%.0f\tmultiplies per output frame (read only)",
                      d->mode == Mode::iir ? 5.0 * d->cascade.sections()
                                           : d->convolver.multiplies());
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
                          // The target, not the cascade: with a curve loaded
                          // the cascade is not the whole of what is realised,
                          // and half a report is worse than none.
                          target_db(*d, hz));
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

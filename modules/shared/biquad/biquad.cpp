// SPDX-License-Identifier: GPL-3.0-or-later

#include "biquad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace mp::biquad {
namespace {

constexpr double k_pi = 3.14159265358979323846;

struct Named {
    Kind kind;
    const char* name;
};

constexpr Named k_kinds[] = {
    {Kind::peak, "peak"},         {Kind::lowshelf, "lowshelf"},
    {Kind::highshelf, "highshelf"}, {Kind::lowpass, "lowpass"},
    {Kind::highpass, "highpass"}, {Kind::bandpass, "bandpass"},
    {Kind::notch, "notch"},       {Kind::allpass, "allpass"},
};

bool takes_gain(Kind kind) noexcept
{
    return kind == Kind::peak || kind == Kind::lowshelf || kind == Kind::highshelf;
}

/// A section's response where z^-1 is `z1` and z^-2 is `z2`.
std::complex<double> response_at(const Coefficients& k, std::complex<double> z1,
                                 std::complex<double> z2) noexcept
{
    return (k.b0 + k.b1 * z1 + k.b2 * z2) / (1.0 + k.a1 * z1 + k.a2 * z2);
}

/// `L` channels through `N` sections, both known when compiling, a sample at
/// a time, with every coefficient and every word of state a local, and the
/// channels' state side by side, so that a section's is one vector.
///
/// **Transposed direct form II, its sums in the order that keeps a section's
/// wait on its own last output to two fused operations**: z0 is
/// (b1 x + z1) - a1 y and z1 is (b2 x) - a2 y, each a fused multiply-add once
/// y is known. (b1 x - a1 y) + z1, as the form is usually written, is three
/// operations after y, a1 y rounded on its own before the other two, and it
/// held a section to one sample every four operations' latency. Two roundings
/// for z0 where there were three: measured against the same filters in long
/// double over MediaPerch's own designs -- peaks, shelves, passes, a notch, an
/// all-pass, a ten-band equaliser and the K-weighting, at 44.1 to 192 kHz, on
/// noise, a sweep and a 20 Hz tone, two minutes of each -- the error is 18 %
/// smaller on the whole and 2.5 times smaller at best, for a 20 Hz low-pass;
/// 27 runs of 224 came out larger, by 4.5 % at most, less than runs of the
/// other orders tried scatter around one another on the same filters.
template <std::size_t N, std::size_t L>
void through_locals(const Coefficients* sections, double* const* state,
                    const double* const* src, double* const* dst, std::uint32_t frames) noexcept
{
    Coefficients k[N];
    double z0[N][L];
    double z1[N][L];
    for (std::size_t s = 0; s < N; ++s) {
        k[s] = sections[s];
        for (std::size_t l = 0; l < L; ++l) {
            z0[s][l] = state[l][s * 2];
            z1[s][l] = state[l][(s * 2) + 1];
        }
    }
    for (std::uint32_t n = 0; n < frames; ++n) {
        double x[L];
        for (std::size_t l = 0; l < L; ++l) {
            x[l] = src[l][n];
        }
        for (std::size_t s = 0; s < N; ++s) {
            for (std::size_t l = 0; l < L; ++l) {
                const double y = k[s].b0 * x[l] + z0[s][l];
                const double t = k[s].b1 * x[l] + z1[s][l];
                const double u = k[s].b2 * x[l];
                z0[s][l] = t - k[s].a1 * y;
                z1[s][l] = u - k[s].a2 * y;
                x[l] = y;
            }
        }
        for (std::size_t l = 0; l < L; ++l) {
            dst[l][n] = x[l];
        }
    }
    for (std::size_t s = 0; s < N; ++s) {
        for (std::size_t l = 0; l < L; ++l) {
            state[l][s * 2] = z0[s][l];
            state[l][(s * 2) + 1] = z1[s][l];
        }
    }
}

/// through_locals for each number of sections a run of them can be, from
/// one, for four channels side by side, for two and for one. Called through
/// these tables, each is a function of its own; a switch let the compiler put
/// all of them in process(), where a function that size kept the lone
/// channel's loop counters on the stack.
using Group = void (*)(const Coefficients*, double* const*, const double* const*,
                       double* const*, std::uint32_t) noexcept;
constexpr Group k_four_side_by_side[] = {
    &through_locals<1, 4>, &through_locals<2, 4>, &through_locals<3, 4>,
    &through_locals<4, 4>, &through_locals<5, 4>, &through_locals<6, 4>,
};
constexpr Group k_two_side_by_side[] = {
    &through_locals<1, 2>, &through_locals<2, 2>, &through_locals<3, 2>,
    &through_locals<4, 2>, &through_locals<5, 2>, &through_locals<6, 2>,
};
constexpr Group k_alone[] = {
    &through_locals<1, 1>, &through_locals<2, 1>, &through_locals<3, 1>,
    &through_locals<4, 1>, &through_locals<5, 1>, &through_locals<6, 1>,
};
static_assert(std::size(k_four_side_by_side) == std::size(k_alone) &&
              std::size(k_two_side_by_side) == std::size(k_alone));

} // namespace

std::complex<double> Coefficients::response(double omega) const noexcept
{
    const std::complex<double> z1{std::cos(-omega), std::sin(-omega)};
    return response_at(*this, z1, z1 * z1);
}

bool kind_from_name(const std::string& name, Kind& out)
{
    for (const Named& named : k_kinds) {
        if (name == named.name) {
            out = named.kind;
            return true;
        }
    }
    return false;
}

const char* kind_name(Kind kind) noexcept
{
    for (const Named& named : k_kinds) {
        if (named.kind == kind) {
            return named.name;
        }
    }
    return "peak";
}

std::string kind_names()
{
    std::string out;
    for (const Named& named : k_kinds) {
        if (!out.empty()) {
            out += ", ";
        }
        out += named.name;
    }
    return out;
}

bool parse_band(const std::string& text, Band& out, std::string& why)
{
    out = Band{};
    std::string body = text;
    if (!body.empty() && body[0] == '-') {
        // A band that is written down and switched off. Deleting it to mute it
        // and typing it again to hear it is how settings get lost.
        out.enabled = false;
        body.erase(0, 1);
    }

    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= body.size()) {
        const std::size_t next = body.find(':', at);
        parts.push_back(body.substr(at, next == std::string::npos ? next : next - at));
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    if (parts.empty() || parts[0].empty()) {
        why = "a band starts with its kind: one of " + kind_names();
        return false;
    }
    if (!kind_from_name(parts[0], out.kind)) {
        why = "`" + parts[0] + "` is not a kind; try one of " + kind_names();
        return false;
    }

    const auto number = [&](std::size_t index, double& target) {
        if (index >= parts.size() || parts[index].empty()) {
            return true; // absent means the default
        }
        const char* start = parts[index].c_str();
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start || !std::isfinite(value)) {
            why = "`" + parts[index] + "` is not a number";
            return false;
        }
        target = value;
        return true;
    };

    if (!number(1, out.frequency_hz)) {
        return false;
    }
    if (!number(2, out.gain_db)) {
        return false;
    }
    if (!number(3, out.q)) {
        return false;
    }
    if (out.frequency_hz <= 0.0) {
        why = "a band needs a frequency above zero";
        return false;
    }
    if (out.q <= 0.0 || out.q > 1000.0) {
        why = "Q has to be above zero and below a thousand";
        return false;
    }
    if (out.gain_db < -60.0 || out.gain_db > 30.0) {
        why = "a band's gain is limited to -60 .. +30 dB";
        return false;
    }
    if (!takes_gain(out.kind)) {
        out.gain_db = 0.0; // a notch has no gain to set, and pretending it does misleads
    }
    return true;
}

bool parse_bands(const std::string& text, std::vector<Band>& out, std::string& why)
{
    out.clear();
    if (text.empty() || text == "none") {
        return true;
    }
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find(';', at);
        const std::string one =
            text.substr(at, next == std::string::npos ? next : next - at);
        if (!one.empty()) {
            Band band;
            if (!parse_band(one, band, why)) {
                return false;
            }
            out.push_back(band);
        }
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    return true;
}

std::string band_text(const Band& band)
{
    char out[128];
    if (takes_gain(band.kind)) {
        std::snprintf(out, sizeof(out), "%s%s:%.4g:%+.4g:%.4g", band.enabled ? "" : "-",
                      kind_name(band.kind), band.frequency_hz, band.gain_db, band.q);
    } else {
        std::snprintf(out, sizeof(out), "%s%s:%.4g::%.4g", band.enabled ? "" : "-",
                      kind_name(band.kind), band.frequency_hz, band.q);
    }
    return out;
}

std::string bands_text(const std::vector<Band>& bands)
{
    std::string out;
    for (const Band& band : bands) {
        if (!out.empty()) {
            out += ';';
        }
        out += band_text(band);
    }
    return out.empty() ? "none" : out;
}

bool design(const Band& band, double sample_rate, Coefficients& out, std::string& why)
{
    if (sample_rate <= 0.0) {
        why = "a filter needs a sample rate";
        return false;
    }
    if (band.frequency_hz >= sample_rate / 2.0) {
        why = band_text(band) + " sits at or above Nyquist for " +
              std::to_string(static_cast<long long>(sample_rate)) +
              " Hz, where it has no analogue to be a filter of";
        return false;
    }

    const double omega = 2.0 * k_pi * band.frequency_hz / sample_rate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / (2.0 * band.q);
    // A for peaking and shelving is the square root of the gain, because the
    // cookbook's shelves reach `gain` and its peaks reach `gain` at the top.
    const double a = std::pow(10.0, band.gain_db / 40.0);

    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;

    switch (band.kind) {
    case Kind::peak:
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosine;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha / a;
        break;
    case Kind::lowshelf: {
        const double root = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) - (a - 1.0) * cosine + root);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosine);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosine - root);
        a0 = (a + 1.0) + (a - 1.0) * cosine + root;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosine);
        a2 = (a + 1.0) + (a - 1.0) * cosine - root;
        break;
    }
    case Kind::highshelf: {
        const double root = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) + (a - 1.0) * cosine + root);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosine - root);
        a0 = (a + 1.0) - (a - 1.0) * cosine + root;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosine);
        a2 = (a + 1.0) - (a - 1.0) * cosine - root;
        break;
    }
    case Kind::lowpass:
        b0 = (1.0 - cosine) / 2.0;
        b1 = 1.0 - cosine;
        b2 = (1.0 - cosine) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
        break;
    case Kind::highpass:
        b0 = (1.0 + cosine) / 2.0;
        b1 = -(1.0 + cosine);
        b2 = (1.0 + cosine) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
        break;
    case Kind::bandpass: // constant skirt gain, peak gain Q
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
        break;
    case Kind::notch:
        b0 = 1.0;
        b1 = -2.0 * cosine;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
        break;
    case Kind::allpass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cosine;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
        break;
    }

    if (a0 == 0.0) {
        why = band_text(band) + " has no stable form at this rate";
        return false;
    }
    out.b0 = b0 / a0;
    out.b1 = b1 / a0;
    out.b2 = b2 / a0;
    out.a1 = a1 / a0;
    out.a2 = a2 / a0;
    return true;
}

bool Cascade::configure(const std::vector<Band>& bands, double sample_rate,
                        std::uint32_t channels, std::string& why)
{
    sections_.clear();
    sample_rate_ = sample_rate;
    channels_ = channels;
    for (const Band& band : bands) {
        if (!band.enabled) {
            continue;
        }
        Coefficients section;
        if (!design(band, sample_rate, section, why)) {
            return false;
        }
        sections_.push_back(section);
    }
    reset();
    return true;
}

void Cascade::set_sections(const std::vector<Coefficients>& sections,
                           std::uint32_t channels)
{
    sections_ = sections;
    channels_ = channels;
    reset();
}

void Cascade::reset() noexcept
{
    state_.assign(static_cast<std::size_t>(channels_) * sections_.size() * 2, 0.0);
}

void Cascade::process(const double* const* in, std::uint32_t frames,
                      double* const* out) noexcept
{
    const std::size_t count = sections_.size();
    if (count == 0) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            if (in[c] != out[c]) {
                std::copy_n(in[c], frames, out[c]);
            }
        }
        return;
    }
    // **A sample at a time through a run of sections, and channels side by
    // side.** A section waits on its own last result, sample after sample, and
    // on the section before it within the sample; the next sample's first
    // section waits on nothing of this sample's last, so the processor has
    // several samples' sections in flight at once. A section at a time over
    // the whole block was measured twice as slow at ten sections for losing
    // that.
    //
    // Four channels, then two, then one, go through a run of up to six
    // sections with every coefficient and every word of state a local; more
    // sections than six are cut into runs as even as they go, each over the
    // whole block before the next. Six sections in flight keep the arithmetic
    // busy, and past six, four channels' state and what a section works with
    // are more vectors than AVX2's sixteen registers: four channels took
    // longer per section in runs of seven and of eight than in runs of five
    // and of six. The state stayed in memory before, where each write to the
    // block could have been to it, so every word of it was stored and loaded
    // again for every sample.
    //
    // **A pair of channels takes the four-channel loop, each channel twice,
    // from three sections up.** The compiler makes vectors of four channels'
    // arithmetic but leaves two channels' to scalar code, judging a vector of
    // two no cheaper. So it is while a sample waits on each section's
    // latency, as it does through one or two sections; from three on, it
    // waits on the number of operations, and four lanes, two of them
    // repeating the other two, were quicker than two scalar channels. The
    // repeated lanes do the same arithmetic on the same values, so they write
    // what the first two write, where the first two write it.
    //
    // Filtering a minute of 48 kHz on AVX2, best of five: two sections, as
    // the K-weighting is, went from 11.7 to 5.6 ms on mono, from 23.7 to
    // 6.2 ms on stereo and from 72.9 to 14.4 ms on 5.1; five from 28.4 to
    // 10.3 ms on stereo and from 86.6 to 20.9 ms on 5.1; ten from 43.1 to
    // 20.5 ms on stereo and from 129.0 to 42.0 ms on 5.1; thirty-one from
    // 160.0 to 61.8 ms on stereo and from 478.1 to 124.6 ms on 5.1.
    const std::size_t runs = (count + std::size(k_alone) - 1) / std::size(k_alone);
    const auto through = [&](std::uint32_t first, std::uint32_t lanes) {
        double* state[4] = {};
        const double* src[4] = {};
        double* dst[4] = {};
        std::size_t done = 0;
        for (std::size_t run = 0; run < runs; ++run) {
            const std::size_t size = (count / runs) + (run < count % runs ? 1 : 0);
            for (std::uint32_t l = 0; l < lanes; ++l) {
                const std::uint32_t channel = first + l;
                state[l] = state_.data() +
                           (((static_cast<std::size_t>(channel) * count) + done) * 2);
                src[l] = run == 0 ? in[channel] : out[channel];
                dst[l] = out[channel];
            }
            const Group* table = lanes == 4   ? k_four_side_by_side
                                 : lanes == 2 ? k_two_side_by_side
                                              : k_alone;
            if (lanes == 2 && size >= 3) {
                state[2] = state[0];
                state[3] = state[1];
                src[2] = src[0];
                src[3] = src[1];
                dst[2] = dst[0];
                dst[3] = dst[1];
                table = k_four_side_by_side;
            }
            table[size - 1](sections_.data() + done, state, src, dst, frames);
            done += size;
        }
    };
    std::uint32_t c = 0;
    for (; c + 4 <= channels_; c += 4) {
        through(c, 4);
    }
    for (; c + 2 <= channels_; c += 2) {
        through(c, 2);
    }
    if (c < channels_) {
        through(c, 1);
    }
}

std::complex<double> Cascade::response(double hz) const noexcept
{
    if (sample_rate_ <= 0.0) {
        return {1.0, 0.0};
    }
    // z^-1 and z^-2 once for every section, where each section worked them
    // out again, a cosine and a sine apiece.
    const double omega = 2.0 * k_pi * hz / sample_rate_;
    const std::complex<double> z1{std::cos(-omega), std::sin(-omega)};
    const std::complex<double> z2 = z1 * z1;
    std::complex<double> total{1.0, 0.0};
    for (const Coefficients& section : sections_) {
        total *= response_at(section, z1, z2);
    }
    return total;
}

double Cascade::magnitude_db(double hz) const noexcept
{
    const double magnitude = std::abs(response(hz));
    return magnitude > 0.0 ? 20.0 * std::log10(magnitude) : -400.0;
}

double Cascade::phase_radians(double hz) const noexcept
{
    return std::arg(response(hz));
}

double Cascade::peak_gain_db(std::uint32_t points) const noexcept
{
    if (sample_rate_ <= 0.0 || points < 2) {
        return 0.0;
    }
    // Logarithmic, because that is where the bands are: a linear sweep spends
    // most of its points above 10 kHz and finds a 40 Hz shelf by luck.
    const double low = std::log(1.0);
    const double high = std::log(sample_rate_ / 2.0);
    double worst = magnitude_db(0.0);
    for (std::uint32_t i = 0; i < points; ++i) {
        const double hz =
            std::exp(low + (high - low) * static_cast<double>(i) / (points - 1));
        worst = std::max(worst, magnitude_db(hz));
    }
    return worst;
}

} // namespace mp::biquad

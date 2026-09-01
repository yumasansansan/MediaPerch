// SPDX-License-Identifier: GPL-3.0-or-later

#include "biquad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

} // namespace

std::complex<double> Coefficients::response(double omega) const noexcept
{
    const std::complex<double> z1{std::cos(-omega), std::sin(-omega)};
    const std::complex<double> z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
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
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const double* src = in[c];
        double* dst = out[c];
        double* state = state_.data() + static_cast<std::size_t>(c) * count * 2;

        if (count == 0) {
            if (src != dst) {
                std::copy_n(src, frames, dst);
            }
            continue;
        }
        for (std::uint32_t n = 0; n < frames; ++n) {
            double x = src[n];
            for (std::size_t s = 0; s < count; ++s) {
                const Coefficients& k = sections_[s];
                double* z = state + s * 2;
                // Transposed direct form II.
                const double y = k.b0 * x + z[0];
                z[0] = k.b1 * x - k.a1 * y + z[1];
                z[1] = k.b2 * x - k.a2 * y;
                x = y;
            }
            dst[n] = x;
        }
    }
}

std::complex<double> Cascade::response(double hz) const noexcept
{
    if (sample_rate_ <= 0.0) {
        return {1.0, 0.0};
    }
    const double omega = 2.0 * k_pi * hz / sample_rate_;
    std::complex<double> total{1.0, 0.0};
    for (const Coefficients& section : sections_) {
        total *= section.response(omega);
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

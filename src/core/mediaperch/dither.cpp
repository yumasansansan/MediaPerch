// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/dither.hpp"

#include "mediaperch/shaper_tables.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace mp {
namespace {

/// c[k-1] = (-1)^k C(N,k), for k = 1..N.
///
/// The binomial expansion of (1 - z^-1)^N without its k = 0 term, negated for
/// the convention this file uses -- the history is added, so the taps carry the
/// sign. With them, the noise transfer function 1 + C(z) is exactly (1 - z^-1)^N.
///
/// Derived rather than tabulated: C(N,k) is exact in `double` for every order
/// this allows, and a table would be a thing to get wrong.
std::vector<double> binomial_taps(std::uint32_t order)
{
    std::vector<double> out;
    out.reserve(order);
    double c = 1.0; // C(N,0)
    for (std::uint32_t k = 1; k <= order; ++k) {
        c = c * static_cast<double>(order - k + 1) / static_cast<double>(k);
        out.push_back((k % 2 == 0) ? c : -c);
    }
    return out;
}

std::vector<double> shaper_taps(const NoiseShaping& shaping, std::uint32_t sample_rate)
{
    switch (shaping.kind) {
    case NoiseShaping::Kind::none:
        return {};
    case NoiseShaping::Kind::binomial:
        return binomial_taps(std::min(shaping.strength, NoiseShaping::k_max_order));
    case NoiseShaping::Kind::shibata:
        if (const ShaperCurve* curve = find_shaper(sample_rate, shaping.strength)) {
            return std::vector<double>{curve->coefficients,
                                       curve->coefficients + curve->length};
        }
        // No curve for this rate. Silently using one fitted for another rate
        // would put the noise an octave from where it belongs, so this is
        // nothing at all, and `taps()` says so.
        return {};
    }
    return {};
}

} // namespace

const ShaperCurve* find_shaper(std::uint32_t sample_rate, std::uint32_t intensity) noexcept
{
    for (const ShaperCurve& curve : shaper_curves()) {
        if (curve.sample_rate == sample_rate && curve.intensity == intensity) {
            return curve.length != 0 ? &curve : nullptr;
        }
    }
    return nullptr;
}

std::uint32_t highest_intensity(std::uint32_t sample_rate) noexcept
{
    std::uint32_t best = 0;
    for (const ShaperCurve& curve : shaper_curves()) {
        // 90 and above are SSRC's special entries -- old curves, a plain
        // first-order shaper, and none -- rather than a continuation of the
        // scale, so they are not what "the strongest" means.
        if (curve.sample_rate == sample_rate && curve.intensity < 90 &&
            curve.intensity > best) {
            best = curve.intensity;
        }
    }
    return best;
}

const char* dither_kind_name(DitherKind kind) noexcept
{
    switch (kind) {
    case DitherKind::none:
        return "none";
    case DitherKind::rectangular:
        return "rectangular";
    case DitherKind::triangular:
        return "triangular";
    case DitherKind::highpass_triangular:
        return "highpass";
    case DitherKind::gaussian:
        return "gaussian";
    }
    return "triangular";
}

bool dither_kind_from_name(std::string_view name, DitherKind& out) noexcept
{
    if (name == "none") {
        out = DitherKind::none;
    } else if (name == "rectangular" || name == "rpdf") {
        out = DitherKind::rectangular;
    } else if (name == "triangular" || name == "tpdf") {
        out = DitherKind::triangular;
    } else if (name == "highpass" || name == "hp-tpdf") {
        out = DitherKind::highpass_triangular;
    } else if (name == "gaussian") {
        out = DitherKind::gaussian;
    } else {
        return false;
    }
    return true;
}

bool noise_shaping_from_name(std::string_view name, NoiseShaping& out) noexcept
{
    auto number = [](std::string_view text, std::uint32_t& value) {
        const char* first = text.data();
        const char* last = first + text.size();
        const auto result = std::from_chars(first, last, value);
        return result.ec == std::errc{} && result.ptr == last;
    };

    if (name.starts_with("shibata")) {
        out.kind = NoiseShaping::Kind::shibata;
        out.strength = 0;
        const std::string_view rest = name.substr(7);
        if (rest.empty()) {
            return true;
        }
        return rest.front() == ':' && number(rest.substr(1), out.strength);
    }

    std::uint32_t order = 0;
    if (!number(name, order) || order > NoiseShaping::k_max_order) {
        return false;
    }
    out.kind = order == 0 ? NoiseShaping::Kind::none : NoiseShaping::Kind::binomial;
    out.strength = order;
    return true;
}

std::string noise_shaping_describe(const NoiseShaping& shaping, std::uint32_t sample_rate)
{
    switch (shaping.kind) {
    case NoiseShaping::Kind::none:
        return "none";
    case NoiseShaping::Kind::binomial:
        return "binomial order " + std::to_string(shaping.strength);
    case NoiseShaping::Kind::shibata:
        if (const ShaperCurve* curve = find_shaper(sample_rate, shaping.strength)) {
            return std::string{"shibata: "} + curve->name + " (" +
                   std::to_string(curve->length) + " taps)";
        }
        return "shibata: no curve for " + std::to_string(sample_rate) +
               " Hz at intensity " + std::to_string(shaping.strength) + ", so none";
    }
    return "none";
}

Dither::Dither(DitherKind kind, NoiseShaping shaping, std::uint32_t sample_rate,
               std::uint32_t seed)
    : kind_(kind), seed_(seed), rng_(seed), taps_(shaper_taps(shaping, sample_rate)),
      history_(taps_.size(), 0.0)
{
}

void Dither::reset() noexcept
{
    rng_ = seed_;
    previous_ = 0.0;
    std::fill(history_.begin(), history_.end(), 0.0);
}

double Dither::uniform() noexcept
{
    // The same generator as everywhere else in this tree, and seeded rather
    // than sampled from a clock: two decodes of one file have to produce the
    // same bytes, or a difference between them means nothing.
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<double>(rng_) / 4294967296.0 - 0.5;
}

double Dither::next() noexcept
{
    switch (kind_) {
    case DitherKind::none:
        return 0.0;
    case DitherKind::rectangular:
        return uniform();
    case DitherKind::triangular:
        // Two independent draws: triangular, white, variance 1/6 LSB^2.
        return uniform() + uniform();
    case DitherKind::highpass_triangular: {
        // One draw, differenced with the last. The distribution is the same
        // triangle -- the difference of two independent uniforms has the same
        // shape as their sum -- but the *spectrum* is (1 - z^-1), so the noise
        // it adds is tilted away from the midband for free.
        const double now = uniform();
        const double out = now - previous_;
        previous_ = now;
        return out;
    }
    case DitherKind::gaussian: {
        // Box-Muller. Sigma is half an LSB, which puts its power between
        // rectangular and triangular while giving the tails a Gaussian shape --
        // the distribution a sum of independent noise sources tends to, and so
        // the one to use when something downstream will quantise again.
        double u1 = uniform() + 0.5;
        const double u2 = uniform() + 0.5;
        u1 = std::max(u1, 1e-12); // log(0)
        return 0.5 * std::sqrt(-2.0 * std::log(u1)) *
               std::cos(2.0 * std::numbers::pi * u2);
    }
    }
    return 0.0;
}

double Dither::feedback() const noexcept
{
    double sum = 0.0;
    for (std::size_t k = 0; k < taps_.size(); ++k) {
        sum += taps_[k] * history_[k];
    }
    return sum;
}

void Dither::accept(double error, bool clipped) noexcept
{
    if (taps_.empty()) {
        return;
    }
    // A shaper is a feedback loop and an overloaded sample hands it an error
    // far larger than an LSB. SSRC bounds the stored value to one LSB when the
    // quantiser clipped, and does not bound it otherwise; the loop is stable
    // for anything inside the rails and this is what keeps it from ringing
    // when the signal is not.
    if (clipped) {
        error = std::clamp(error, -1.0, 1.0);
    }

    for (std::size_t k = history_.size(); k > 1; --k) {
        history_[k - 1] = history_[k - 2];
    }
    history_[0] = error;
}

} // namespace mp

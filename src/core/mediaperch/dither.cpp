// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/dither.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mp {
namespace {

/// h[k] = (-1)^(k+1) * C(N,k), for k = 1..N.
///
/// Which is the binomial expansion of (1 - z^-1)^N with the k = 0 term removed,
/// because that term is the sample itself rather than a feedback tap. Derived
/// here rather than tabulated: C(N,k) is exact in `double` for every N this
/// allows, and a table would be a thing to get wrong.
void binomial_taps(std::uint32_t order, std::array<double, NoiseShaping::k_max_order>& out)
{
    out.fill(0.0);
    double c = 1.0; // C(N,0)
    for (std::uint32_t k = 1; k <= order; ++k) {
        c = c * static_cast<double>(order - k + 1) / static_cast<double>(k);
        out[k - 1] = (k % 2 == 1) ? c : -c;
    }
}

} // namespace

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

Dither::Dither(DitherKind kind, NoiseShaping shaping, std::uint32_t seed) noexcept
    : kind_(kind), order_(std::min(shaping.order, NoiseShaping::k_max_order)), seed_(seed),
      rng_(seed)
{
    binomial_taps(order_, taps_);
}

void Dither::reset() noexcept
{
    rng_ = seed_;
    previous_ = 0.0;
    history_.fill(0.0);
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
    // u[n] = x[n] - sum h[k] e[n-k], which makes the noise transfer function
    // 1 - H(z) = (1 - z^-1)^N exactly.
    double sum = 0.0;
    for (std::uint32_t k = 0; k < order_; ++k) {
        sum += taps_[k] * history_[k];
    }
    return sum;
}

void Dither::accept(double error) noexcept
{
    if (order_ == 0) {
        return;
    }
    // A shaper is a feedback loop and a feedback loop can run away: a clipped
    // sample produces an error far larger than an LSB, and a high-order filter
    // multiplies it. Bounding what enters the history costs nothing when the
    // signal is inside the rails and stops the loop ringing when it is not.
    constexpr double k_bound = 4.0;
    error = std::clamp(error, -k_bound, k_bound);

    for (std::uint32_t k = order_; k > 1; --k) {
        history_[k - 1] = history_[k - 2];
    }
    history_[0] = error;
}

} // namespace mp

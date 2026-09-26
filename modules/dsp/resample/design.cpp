// SPDX-License-Identifier: GPL-3.0-or-later

#include "design.hpp"

#include <transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mp::resample {

// Moved to modules/transform when the equaliser wanted them too.
using mp::transform::dft_any;
using mp::transform::kaiser_beta;
using mp::transform::next_power_of_two;
using mp::transform::to_minimum_phase;

namespace {

constexpr double k_pi = 3.14159265358979323846;

/// `count` values into `low`, and the same values in reverse order into
/// `high`: the two halves of a symmetric prototype, the second made from the
/// first. Two loops of neighbours, and one of them reading backwards from a
/// block of its own -- where writing tap n and its mirror in one loop is
/// writing the one array in two places, which may meet at the centre, and
/// that the compiler cannot make vector code of.
void mirror(const double* values, std::size_t count, double* low, double* high) noexcept
{
    std::copy_n(values, count, low);
    for (std::size_t i = 0; i < count; ++i) {
        high[i] = values[count - 1 - i];
    }
}

/// Makes `h`, of `length` taps, exactly symmetric: each tap and its mirror
/// both their mean. The two halves are separate ranges and are passed as such
/// -- `__restrict` says so -- or the compiler must allow that a write to one
/// half lands in the other, and the loop stays scalar.
void symmetric_halves(double* __restrict low, double* __restrict high, std::size_t half) noexcept
{
    for (std::size_t i = 0; i < half; ++i) {
        const double mean = 0.5 * (low[i] + high[half - 1 - i]);
        low[i] = mean;
        high[half - 1 - i] = mean;
    }
}

void symmetrize(std::vector<double>& h) noexcept
{
    const std::size_t half = h.size() / 2;
    symmetric_halves(h.data(), h.data() + (h.size() - half), half);
}

/// The window method's sinc, sin(pi x) / (pi x), at x = j / m for whole j: the
/// ideal lowpass whose cutoff is half of 1/m.
///
/// **The sine is taken from a table of a quarter of its period.** sin(pi j / m)
/// repeats every 2m and mirrors about m / 2, so the whole of it is m / 2 + 1
/// values, reached from any j by whole-number arithmetic: a sine per entry
/// rather than per tap -- 27 thousand for a filter of 8 million taps -- and each
/// of a small angle. The sine of pi j / m taken directly is of an angle rounded
/// at its full size, which at the far end of a long filter is an error of 4e-14
/// in a value that may be much smaller than one; from the table it is an ulp or
/// two, and every multiple of m is a true zero.
class Sinc {
public:
    /// For j up to `most`.
    Sinc(std::uint64_t m, std::uint64_t most) : m_(m)
    {
        const std::uint64_t entries = std::min(m / 2, most) + 1;
        quarter_.resize(static_cast<std::size_t>(entries));
        for (std::uint64_t k = 0; k < entries; ++k) {
            quarter_[static_cast<std::size_t>(k)] =
                std::sin(k_pi * (static_cast<double>(k) / static_cast<double>(m)));
        }
    }

    /// sinc(j / m) for j = `from`, `from` - 1, and so on, `count` of them, into
    /// `out`. Counting down is what the window method's half does.
    void down(std::uint64_t from, std::size_t count, double* out) const noexcept
    {
        // The sines first. The place in the period counts down by one a tap
        // and wraps where it passes zero, which is at most every two m taps,
        // so the taps are runs in each of which the place is a plain
        // difference -- a loop of whole-number arithmetic and a table read,
        // with no division and no branch in it.
        const std::uint64_t period = 2 * m_;
        std::uint64_t at = from % period;
        for (std::size_t done = 0; done < count;) {
            const auto run =
                static_cast<std::size_t>(std::min<std::uint64_t>(count - done, at + 1));
            for (std::size_t r = 0; r < run; ++r) {
                // The second half of the period is the first, negated; within
                // a half, the sine mirrors about its middle.
                const std::uint64_t place = at - r;
                const bool negative = place >= m_;
                const std::uint64_t within = negative ? place - m_ : place;
                const std::uint64_t k = 2 * within > m_ ? m_ - within : within;
                const double s = quarter_[static_cast<std::size_t>(k)];
                out[done + r] = negative ? -s : s;
            }
            done += run;
            at = period - 1;
        }
        // Then over pi x, x = j / m. j is below 2^31 -- a prototype has fewer
        // than 2^32 taps and j is at most half of them -- so it is counted in
        // 32 bits, which the vector unit turns into doubles, where before
        // AVX-512 it has no instruction for 64.
        const auto first = static_cast<std::int32_t>(from);
        const auto m = static_cast<double>(m_);
        for (std::size_t r = 0; r < count; ++r) {
            const std::int32_t j = first - static_cast<std::int32_t>(r);
            const double t = k_pi * (static_cast<double>(j) / m);
            out[r] = j == 0 ? 1.0 : out[r] / t;
        }
    }

private:
    std::uint64_t m_;
    std::vector<double> quarter_;
};

/// How many eigenvalues of the symmetric tridiagonal (d, e) are below `x`.
///
/// The Sturm sequence, which is the whole of a bisection eigensolver: one
/// division per row, no matrix, no allocation.
std::size_t below(const std::vector<double>& d, const std::vector<double>& e, double x)
{
    constexpr double k_tiny = 1e-300;
    std::size_t count = 0;
    double q = d[0] - x;
    if (q < 0.0) {
        ++count;
    }
    for (std::size_t k = 1; k < d.size(); ++k) {
        if (q == 0.0) {
            q = k_tiny;
        }
        q = (d[k] - x) - e[k] * e[k] / q;
        if (q < 0.0) {
            ++count;
        }
    }
    return count;
}

/// acosh(1 + e), given `e` rather than `1 + e`.
///
/// **This is the whole difficulty of a long Dolph window.** For a 25,281-tap
/// filter at 120 dB the window's parameter is 1 + 1.6e-7, and forming that sum
/// before taking the arc-cosh throws away half the significant digits of the
/// part that matters -- which comes out as a window that is 15 dB worse than it
/// was asked to be, in a way that looks like the method rather than the
/// arithmetic. Passing the small part in undamaged is the fix.
double acosh1p(double e) noexcept
{
    return std::log1p(e + std::sqrt(e * (2.0 + e)));
}

double square(double x) noexcept
{
    return x * x;
}

} // namespace

// --------------------------------------------------------------------------
// Windows
// --------------------------------------------------------------------------

double kaiser_beta_for(double attenuation_db) noexcept
{
    return kaiser_beta(attenuation_db);
}

std::vector<double> kaiser_window(std::size_t length, double attenuation_db)
{
    return mp::transform::kaiser_window(length, kaiser_beta(attenuation_db));
}

std::vector<double> dolph_window(std::size_t length, double attenuation_db)
{
    if (length < 2) {
        return std::vector<double>(length, 1.0);
    }
    const double order = static_cast<double>(length - 1);
    const double ripple = std::pow(10.0, attenuation_db / 20.0);
    // beta = cosh(acosh(ripple)/order), kept as its distance from one: for a
    // long filter that distance is a ten-millionth and the one is what would
    // destroy it.
    const double u = std::acosh(ripple) / order;
    const double beta_minus_one = 2.0 * square(std::sinh(u / 2.0));
    const double beta = 1.0 + beta_minus_one;

    std::vector<std::complex<double>> p(length);
    const bool odd = (length % 2) == 1;
    for (std::size_t k = 0; k < length; ++k) {
        const double theta = k_pi * static_cast<double>(k) / static_cast<double>(length);
        const double phi = k_pi - theta;
        // beta*cos(theta) - 1 and -beta*cos(theta) - 1, both without ever
        // forming a number near one and subtracting one from it.
        const double above = beta_minus_one * std::cos(theta) -
                             2.0 * square(std::sin(theta / 2.0));
        const double below =
            beta_minus_one * std::cos(phi) - 2.0 * square(std::sin(phi / 2.0));

        double value = 0.0;
        if (above > 0.0) {
            value = std::cosh(order * acosh1p(above));
        } else if (below > 0.0) {
            value = std::cosh(order * acosh1p(below));
            if (!odd) {
                value = -value; // T_n(-x) = (-1)^n T_n(x), and n is odd here
            }
        } else {
            value = std::cos(order * std::acos(beta * std::cos(theta)));
        }
        p[k] = {value, 0.0};
        if (!odd) {
            // An even-length window sits half a sample off the grid, and this
            // is the shift that puts it back.
            const double phase = k_pi * static_cast<double>(k) / static_cast<double>(length);
            p[k] *= std::complex<double>{std::cos(phase), std::sin(phase)};
        }
    }
    dft_any(p);

    std::vector<double> w(length, 0.0);
    if (odd) {
        const std::size_t half = (length + 1) / 2;
        for (std::size_t i = 0; i < half; ++i) {
            w[half - 1 + i] = p[i].real();
            w[half - 1 - i] = p[i].real();
        }
    } else {
        const std::size_t half = length / 2;
        for (std::size_t i = 0; i < half; ++i) {
            w[half + i] = p[i].real();
            w[half - 1 - i] = p[i].real();
        }
    }
    const double peak = *std::max_element(w.begin(), w.end());
    if (peak != 0.0) {
        for (double& value : w) {
            value /= peak;
        }
    }
    return w;
}

std::vector<double> dpss_window(std::size_t length, double nw)
{
    if (length < 4 || nw <= 0.0) {
        return std::vector<double>(length, 1.0);
    }

    // Percival and Walden's tridiagonal, whose eigenvectors are the DPSS
    // exactly. Solving this rather than the concentration problem itself is the
    // difference between an O(n) computation and an O(n^3) one, and it is what
    // makes a Slepian window available at 25,281 taps at all.
    const double w = nw / static_cast<double>(length);
    const auto n = static_cast<double>(length);
    std::vector<double> d(length);
    std::vector<double> e(length, 0.0);
    for (std::size_t k = 0; k < length; ++k) {
        const double centre = (n - 1.0 - 2.0 * static_cast<double>(k)) / 2.0;
        d[k] = centre * centre * std::cos(2.0 * k_pi * w);
        if (k > 0) {
            e[k] = static_cast<double>(k) * (n - static_cast<double>(k)) / 2.0;
        }
    }

    // Gershgorin, then bisection for the largest eigenvalue. The zeroth DPSS is
    // the eigenvector that belongs to it.
    double lo = d[0];
    double hi = d[0];
    for (std::size_t k = 0; k < length; ++k) {
        const double radius =
            std::abs(e[k]) + (k + 1 < length ? std::abs(e[k + 1]) : 0.0);
        lo = std::min(lo, d[k] - radius);
        hi = std::max(hi, d[k] + radius);
    }
    const double scale = std::max(std::abs(lo), std::abs(hi));
    for (int i = 0; i < 200 && (hi - lo) > 1e-15 * scale; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (below(d, e, mid) >= length) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const double lambda = 0.5 * (lo + hi);

    // Inverse iteration. The shift is a hair off the eigenvalue on purpose:
    // exactly on it the system is singular, and a hair off it is what makes the
    // solve project onto the eigenvector in one step.
    std::vector<double> v(length, 1.0);
    std::vector<double> c(length);
    std::vector<double> u(length);
    const double shift = lambda - 1e-9 * std::max(1.0, std::abs(lambda));
    for (int iteration = 0; iteration < 6; ++iteration) {
        // Thomas, with a guard for the pivot that a near-singular system will
        // produce.
        double beta = d[0] - shift;
        if (std::abs(beta) < 1e-300) {
            beta = 1e-300;
        }
        u[0] = v[0] / beta;
        for (std::size_t k = 1; k < length; ++k) {
            c[k] = e[k] / beta;
            beta = (d[k] - shift) - e[k] * c[k];
            if (std::abs(beta) < 1e-300) {
                beta = 1e-300;
            }
            u[k] = (v[k] - e[k] * u[k - 1]) / beta;
        }
        for (std::size_t k = length - 1; k-- > 0;) {
            u[k] -= c[k + 1] * u[k + 1];
        }
        double norm = 0.0;
        for (const double value : u) {
            norm += value * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0 || !std::isfinite(norm)) {
            break;
        }
        for (std::size_t k = 0; k < length; ++k) {
            v[k] = u[k] / norm;
        }
    }

    // The zeroth DPSS is positive everywhere; inverse iteration may hand back
    // its negative.
    double sum = 0.0;
    for (const double value : v) {
        sum += value;
    }
    if (sum < 0.0) {
        for (double& value : v) {
            value = -value;
        }
    }
    const double peak = *std::max_element(v.begin(), v.end());
    if (peak > 0.0) {
        for (double& value : v) {
            value = std::max(0.0, value) / peak;
        }
    }
    return v;
}

// --------------------------------------------------------------------------
// Measuring
// --------------------------------------------------------------------------

namespace {

/// Which bins of a transform of `n` points are in which band: the passband is
/// bins [0, pass_end) and the stopband bins [stop_begin, n / 2].
struct Bands {
    std::size_t pass_end = 0;
    std::size_t stop_begin = 0;
};

/// Bin k is at k / n of the rate, exactly, n being a power of two: so it is in
/// the passband when k <= passband_edge * n, which is exact too, and in the
/// stopband when k >= stopband_edge * n. The bands are ranges of bins, then,
/// rather than a division and two comparisons for every one of them -- and a
/// bin that is somehow in both is in the passband, as it always was.
Bands bands(std::size_t n, double passband_edge, double stopband_edge) noexcept
{
    const std::size_t last = n / 2;
    const double pass_at = passband_edge * static_cast<double>(n);
    const double stop_at = stopband_edge * static_cast<double>(n);
    Bands out;
    out.pass_end = !(pass_at >= 0.0)
                       ? 0
                       : static_cast<std::size_t>(
                             std::min(std::floor(pass_at) + 1.0, static_cast<double>(last + 1)));
    out.stop_begin = std::max(
        out.pass_end, !(stop_at <= static_cast<double>(last))
                          ? last + 1
                          : static_cast<std::size_t>(std::max(0.0, std::ceil(stop_at))));
    return out;
}

/// What `measure` does, with the transform planned once and the room for it
/// kept: refining measures a filter of the same length every round.
class Meter {
public:
    Meter(std::size_t taps, std::size_t most_points)
        : n_(points_for(taps, most_points)), transform_(n_), folded_(n_), spectrum_(n_ / 2 + 1)
    {
    }

    Response operator()(const std::vector<double>& h, double passband_edge,
                        double stopband_edge, double gain)
    {
        Response out;
        if (h.empty() || gain == 0.0) {
            return out;
        }
        const std::size_t n = n_;
        // Folded rather than truncated when the filter is longer than the
        // transform: aliasing in time is exact sampling in frequency, which is
        // the right approximation to make here. A transform's length at a time,
        // each tap added where it lands, in the order the taps come.
        std::fill(folded_.begin(), folded_.end(), 0.0);
        for (std::size_t at = 0; at < h.size(); at += n) {
            const std::size_t count = std::min(n, h.size() - at);
            for (std::size_t i = 0; i < count; ++i) {
                folded_[i] += h[at + i];
            }
        }
        // The filter is real, so half the transform, and half the bins.
        transform_.forward(folded_.data(), spectrum_.data());

        // The transition band between the two is not looked at, as it never
        // was.
        const std::size_t last = n / 2;
        const Bands band = bands(n, passband_edge, stopband_edge);

        out.points = last;
        double worst_pass = 0.0;
        for (std::size_t k = 0; k < band.pass_end; ++k) {
            const double magnitude = std::abs(spectrum_[k]) / gain;
            worst_pass = std::max(worst_pass, std::abs(magnitude - 1.0));
        }
        // The largest magnitude, divided by the gain once: a division rounds
        // in the same direction as its dividend grows, so the largest quotient
        // is the quotient of the largest.
        double peak = 0.0;
        for (std::size_t k = band.stop_begin; k <= last; ++k) {
            peak = std::max(peak, std::abs(spectrum_[k]));
        }
        const double worst_stop = peak / gain;
        out.passband_ripple_db = 20.0 * std::log10(1.0 + worst_pass);
        out.stopband_db = worst_stop > 0.0 ? 20.0 * std::log10(worst_stop) : -400.0;
        return out;
    }

private:
    // Eight samples per ripple is enough to find the peaks and not so many that
    // a million-tap prototype takes a second to check. The cap is what keeps
    // this honest for the very longest filters, and `points` is reported so a
    // caller can see the resolution it got.
    static std::size_t points_for(std::size_t taps, std::size_t most_points) noexcept
    {
        const std::size_t want = next_power_of_two(std::max<std::size_t>(taps * 8, 4096));
        return std::min<std::size_t>(want,
                                     next_power_of_two(std::max<std::size_t>(most_points, 4096)));
    }

    std::size_t n_;
    mp::transform::RealFft transform_;
    std::vector<double> folded_;
    std::vector<std::complex<double>> spectrum_;
};

} // namespace

Response measure(const std::vector<double>& h, double passband_edge, double stopband_edge,
                 double gain, std::size_t most_points)
{
    if (h.empty() || gain == 0.0) {
        return {};
    }
    Meter meter(h.size(), most_points);
    return meter(h, passband_edge, stopband_edge, gain);
}

// --------------------------------------------------------------------------
// Parks-McClellan
// --------------------------------------------------------------------------

namespace {

/// The barycentric weight for node `k`, computed the way the original Fortran
/// did it: strided, and with a factor of two per term, so the product stays
/// near one instead of underflowing after a few hundred nodes.
double barycentric(const std::vector<double>& x, std::size_t k, std::size_t count)
{
    constexpr std::size_t k_stride = 15;
    double d = 1.0;
    const double q = x[k];
    for (std::size_t l = 0; l < k_stride; ++l) {
        for (std::size_t j = l; j < count; j += k_stride) {
            if (j != k) {
                d *= 2.0 * (q - x[j]);
            }
        }
    }
    return 1.0 / d;
}

} // namespace

bool remez_lowpass(std::size_t length, double passband_edge, double stopband_edge,
                   double weight_pass, double weight_stop, std::vector<double>& h,
                   double& deviation, std::string& why)
{
    if (length % 2 == 0) {
        why = "Parks-McClellan here designs a Type I filter, which has an odd length";
        return false;
    }
    const std::size_t r = (length + 1) / 2; // cosine terms
    if (r < 3) {
        why = "too short to design";
        return false;
    }
    const std::size_t extremals = r + 1;

    // The grid. Denser in proportion to the band, and both edges are on it: the
    // extrema of a lowpass sit at the band edges and an exchange that cannot
    // reach them will not converge.
    const double pass_width = passband_edge;
    const double stop_width = 0.5 - stopband_edge;
    const double total = pass_width + stop_width;
    const std::size_t density = 16;
    const std::size_t points = density * extremals;
    const auto pass_points =
        std::max<std::size_t>(4, static_cast<std::size_t>(static_cast<double>(points) *
                                                          pass_width / total));
    const std::size_t stop_points = std::max<std::size_t>(4, points - pass_points);

    std::vector<double> grid;
    std::vector<double> desired;
    std::vector<double> weight;
    grid.reserve(pass_points + stop_points);
    for (std::size_t i = 0; i < pass_points; ++i) {
        grid.push_back(pass_width * static_cast<double>(i) /
                       static_cast<double>(pass_points - 1));
        desired.push_back(1.0);
        weight.push_back(weight_pass);
    }
    for (std::size_t i = 0; i < stop_points; ++i) {
        grid.push_back(stopband_edge + stop_width * static_cast<double>(i) /
                                           static_cast<double>(stop_points - 1));
        desired.push_back(0.0);
        weight.push_back(weight_stop);
    }
    const std::size_t grid_size = grid.size();
    if (grid_size <= extremals) {
        why = "the grid is smaller than the filter, which cannot happen for a real design";
        return false;
    }

    std::vector<double> cosine(grid_size);
    for (std::size_t i = 0; i < grid_size; ++i) {
        cosine[i] = std::cos(2.0 * k_pi * grid[i]);
    }

    // Start from extremals spread evenly over the grid.
    std::vector<std::size_t> at(extremals);
    for (std::size_t i = 0; i < extremals; ++i) {
        at[i] = i * (grid_size - 1) / (extremals - 1);
    }

    std::vector<double> x(extremals);
    std::vector<double> gamma(extremals);
    std::vector<double> y(extremals);
    std::vector<double> error(grid_size);
    double last = -1.0;

    for (int iteration = 0; iteration < 64; ++iteration) {
        for (std::size_t i = 0; i < extremals; ++i) {
            x[i] = cosine[at[i]];
        }
        for (std::size_t i = 0; i < extremals; ++i) {
            gamma[i] = barycentric(x, i, extremals);
        }

        double numerator = 0.0;
        double denominator = 0.0;
        double sign = 1.0;
        for (std::size_t i = 0; i < extremals; ++i) {
            numerator += gamma[i] * desired[at[i]];
            denominator += sign * gamma[i] / weight[at[i]];
            sign = -sign;
        }
        if (denominator == 0.0 || !std::isfinite(numerator) || !std::isfinite(denominator)) {
            why = "the exchange lost conditioning; this length is past what double "
                  "precision supports for Parks-McClellan";
            return false;
        }
        deviation = numerator / denominator;

        // Interpolate through the first r extremals, which is what the
        // alternation theorem leaves once the deviation is known.
        sign = 1.0;
        for (std::size_t i = 0; i < extremals; ++i) {
            y[i] = desired[at[i]] - sign * deviation / weight[at[i]];
            sign = -sign;
        }
        std::vector<double> nodes(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(r));
        std::vector<double> node_weight(r);
        for (std::size_t i = 0; i < r; ++i) {
            node_weight[i] = barycentric(nodes, i, r);
        }

        for (std::size_t g = 0; g < grid_size; ++g) {
            double top = 0.0;
            double bottom = 0.0;
            double exact = std::numeric_limits<double>::quiet_NaN();
            for (std::size_t i = 0; i < r; ++i) {
                const double difference = cosine[g] - nodes[i];
                if (difference == 0.0) {
                    exact = y[i];
                    break;
                }
                const double term = node_weight[i] / difference;
                top += term * y[i];
                bottom += term;
            }
            const double value = std::isnan(exact) ? top / bottom : exact;
            error[g] = weight[g] * (desired[g] - value);
        }

        // The new extremal set. Two rules, and both matter: a candidate is a
        // turning point of the *signed* error (or a band edge, where the grid
        // stops and the error cannot turn), and the set has to alternate in
        // sign -- that is what the alternation theorem is about, and a set
        // collected by magnitude alone converges to something that is not the
        // answer.
        std::vector<std::size_t> found;
        found.reserve(extremals + 16);
        const auto scan = [&](std::size_t lo, std::size_t hi) {
            for (std::size_t g = lo; g <= hi; ++g) {
                bool candidate = (g == lo || g == hi);
                if (!candidate) {
                    candidate = (error[g] >= error[g - 1] && error[g] >= error[g + 1]) ||
                                (error[g] <= error[g - 1] && error[g] <= error[g + 1]);
                }
                if (!candidate) {
                    continue;
                }
                if (!found.empty() && (error[g] > 0.0) == (error[found.back()] > 0.0)) {
                    // Same sign as the one before it: they are the same
                    // alternation, so keep whichever is further out.
                    if (std::abs(error[g]) > std::abs(error[found.back()])) {
                        found.back() = g;
                    }
                    continue;
                }
                found.push_back(g);
            }
        };
        scan(0, pass_points - 1);
        scan(pass_points, grid_size - 1);

        if (found.size() < extremals) {
            why = "the exchange found " + std::to_string(found.size()) +
                  " alternations where the filter has " + std::to_string(extremals) +
                  " degrees of freedom, which means it did not converge";
            return false;
        }
        // Too many: drop from whichever end is further in, which is the only
        // removal that leaves the alternation intact.
        while (found.size() > extremals) {
            if (std::abs(error[found.front()]) < std::abs(error[found.back()])) {
                found.erase(found.begin());
            } else {
                found.pop_back();
            }
        }

        double peak = 0.0;
        double trough = std::numeric_limits<double>::infinity();
        for (const std::size_t g : found) {
            peak = std::max(peak, std::abs(error[g]));
            trough = std::min(trough, std::abs(error[g]));
        }
        at = found;
        if (peak > 0.0 && (peak - trough) / peak < 1e-8) {
            break; // equiripple to the precision this can measure
        }
        if (last > 0.0 && std::abs(peak - last) < 1e-15) {
            break;
        }
        last = peak;
    }

    // The cosine coefficients, from the interpolant sampled on a full circle.
    const std::size_t samples = 2 * r - 1;
    std::vector<double> amplitude(samples);
    {
        for (std::size_t i = 0; i < extremals; ++i) {
            x[i] = cosine[at[i]];
        }
        for (std::size_t i = 0; i < extremals; ++i) {
            gamma[i] = barycentric(x, i, extremals);
        }
        double sign = 1.0;
        for (std::size_t i = 0; i < extremals; ++i) {
            y[i] = desired[at[i]] - sign * deviation / weight[at[i]];
            sign = -sign;
        }
        std::vector<double> nodes(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(r));
        std::vector<double> node_weight(r);
        for (std::size_t i = 0; i < r; ++i) {
            node_weight[i] = barycentric(nodes, i, r);
        }
        for (std::size_t m = 0; m < samples; ++m) {
            const double omega =
                2.0 * k_pi * static_cast<double>(m) / static_cast<double>(samples);
            const double xm = std::cos(omega);
            double top = 0.0;
            double bottom = 0.0;
            double exact = std::numeric_limits<double>::quiet_NaN();
            for (std::size_t i = 0; i < r; ++i) {
                const double difference = xm - nodes[i];
                if (difference == 0.0) {
                    exact = y[i];
                    break;
                }
                const double term = node_weight[i] / difference;
                top += term * y[i];
                bottom += term;
            }
            amplitude[m] = std::isnan(exact) ? top / bottom : exact;
        }
    }

    std::vector<double> a(r, 0.0);
    for (std::size_t n = 0; n < r; ++n) {
        double sum = amplitude[0];
        for (std::size_t m = 1; m < samples; ++m) {
            sum += amplitude[m] * std::cos(2.0 * k_pi * static_cast<double>(m) *
                                           static_cast<double>(n) /
                                           static_cast<double>(samples));
        }
        a[n] = sum / static_cast<double>(samples);
    }

    h.assign(length, 0.0);
    const std::size_t centre = r - 1;
    // A(w) = a0 + 2 sum a_k cos(kw), and the inverse transform above already
    // returns half of each a_k for k >= 1 -- the orthogonality of the cosines
    // over 2r-1 points puts the factor there. Halving again, which is what the
    // textbook mapping h[centre+k] = a_k/2 says, halves it twice.
    h[centre] = a[0];
    for (std::size_t n = 1; n < r; ++n) {
        h[centre + n] = a[n];
        h[centre - n] = a[n];
    }
    return true;
}

// --------------------------------------------------------------------------
// The prototype
// --------------------------------------------------------------------------

namespace {

/// The rotation that undoes a symmetric filter's delay, bin by bin, for a filter
/// of `length` taps in a real transform of `transform` points: its bins 0 to
/// `transform / 2`.
///
/// It depends on nothing a round of refining changes, so it is made once for
/// all of them: a cosine and a sine for each of a quarter of a million bins,
/// sixty rounds over, was most of what refining cost.
std::vector<std::complex<double>> rotations(std::size_t length, std::size_t transform)
{
    const std::uint64_t centre = (length - 1) / 2;
    std::vector<std::complex<double>> out(transform / 2 + 1);
    for (std::size_t k = 0; k < out.size(); ++k) {
        // The delay is `centre` samples and k can be a million, so the turns are
        // counted in whole numbers, modulo a whole turn, before any angle is.
        out[k] = mp::transform::unit(static_cast<std::size_t>((k * centre) % transform),
                                     transform);
    }
    return out;
}

/// One round of alternating projection: clip the response to the target, then
/// put the filter back to its own length.
///
/// A filter is two constraints -- it is `length` taps long, and its response is
/// inside the mask. Projecting onto each in turn converges to a point in both,
/// which is the same answer Parks-McClellan gives and is reached with two FFTs
/// per round instead of an interpolation over ten thousand nodes. That is the
/// whole reason this exists: it is the only method here that works at the length
/// 44100 -> 48000 actually asks for.
///
/// The filter is real, so the transforms are: `transform` is planned for the
/// length the response is looked at with, `rotate` is its bins' rotations (see
/// `rotations`), and `signal` and `spectrum` are room the caller keeps from one
/// round to the next.
void project(std::vector<double>& h, mp::transform::RealFft& transform,
             const std::vector<std::complex<double>>& rotate, std::vector<double>& signal,
             std::vector<std::complex<double>>& spectrum, double passband_edge,
             double stopband_edge, double gain, double target_pass, double target_stop)
{
    const std::size_t length = h.size();
    const std::size_t n = transform.size();

    signal.assign(n, 0.0);
    std::copy(h.begin(), h.end(), signal.begin());
    spectrum.resize(n / 2 + 1);
    transform.forward(signal.data(), spectrum.data());

    // The zero-phase amplitude: undo the delay, which for a symmetric filter
    // leaves a real number; clip it; and put the delay back. The bands are
    // ranges of bins (see `bands`), and each range is a loop of plain
    // arithmetic.
    const std::size_t last = n / 2;
    const Bands band = bands(n, passband_edge, stopband_edge);
    std::complex<double>* bins = spectrum.data();
    const std::complex<double>* turn = rotate.data();
    const auto clip = [&](std::size_t from, std::size_t to, double low, double high) {
        for (std::size_t k = from; k < to; ++k) {
            const double rr = turn[k].real();
            const double ri = turn[k].imag();
            const double value = std::clamp(
                (bins[k].real() * rr - bins[k].imag() * ri) / gain, low, high);
            const double amplitude = value * gain;
            bins[k] = {amplitude * rr, -(amplitude * ri)};
        }
    };
    const double unclipped = std::numeric_limits<double>::infinity();
    clip(0, band.pass_end, 1.0 - target_pass, 1.0 + target_pass);
    clip(band.pass_end, band.stop_begin, -unclipped, unclipped);
    clip(band.stop_begin, last + 1, -target_stop, target_stop);

    transform.inverse(spectrum.data(), signal.data());
    std::copy_n(signal.begin(), length, h.begin());
    // Symmetry is a constraint too, and floating point does not preserve it for
    // free.
    symmetrize(h);
}

} // namespace

bool method_from_name(const std::string& name, Method& out)
{
    if (name == "window") {
        out = Method::window;
    } else if (name == "remez") {
        out = Method::remez;
    } else if (name == "refine") {
        out = Method::refine;
    } else {
        return false;
    }
    return true;
}

bool window_from_name(const std::string& name, Window& out)
{
    if (name == "kaiser") {
        out = Window::kaiser;
    } else if (name == "dolph" || name == "chebyshev") {
        out = Window::dolph;
    } else if (name == "dpss" || name == "slepian") {
        out = Window::dpss;
    } else {
        return false;
    }
    return true;
}

bool phase_from_name(const std::string& name, Phase& out)
{
    if (name == "linear") {
        out = Phase::linear;
    } else if (name == "minimum" || name == "min") {
        out = Phase::minimum;
    } else {
        return false;
    }
    return true;
}

const char* phase_name(Phase p) noexcept
{
    return p == Phase::minimum ? "minimum" : "linear";
}

const char* method_name(Method m) noexcept
{
    switch (m) {
    case Method::remez:
        return "remez";
    case Method::refine:
        return "refine";
    case Method::window:
        break;
    }
    return "window";
}

const char* window_name(Window w) noexcept
{
    switch (w) {
    case Window::dolph:
        return "dolph";
    case Window::dpss:
        return "dpss";
    case Window::kaiser:
        break;
    }
    return "kaiser";
}

bool design_prototype(const Design& design, std::uint32_t up, std::uint32_t down,
                      std::vector<double>& out, std::uint32_t& taps, Response& achieved,
                      std::string& why)
{
    if (up == 0 || down == 0) {
        why = "a ratio of zero is not a ratio";
        return false;
    }
    if (design.bandwidth <= 0.0 || design.bandwidth >= 1.0) {
        why = "bandwidth must be between 0 and 1, exclusive";
        return false;
    }

    // The lower of the two Nyquist frequencies, in cycles per sample of the
    // intermediate rate. Upsampling must not let images through and
    // downsampling must not let anything fold back; this protects both.
    const double cutoff = 0.5 / static_cast<double>(std::max(up, down));
    const double transition = 2.0 * (1.0 - design.bandwidth) * cutoff;
    const double passband_edge = cutoff - transition / 2.0;
    const double stopband_edge = cutoff + transition / 2.0;
    const double gain = static_cast<double>(up);

    std::uint64_t chosen = design.taps;
    if (chosen == 0) {
        // Kaiser's order estimate, which is also a fair starting point for the
        // others: it is the number of taps the transition band costs, and no
        // method escapes that. Parks-McClellan then beats the estimate rather
        // than needing a different one.
        const double order =
            (design.attenuation_db - 8.0) / (2.285 * 2.0 * k_pi * transition) + 1.0;
        const double estimate = std::ceil(order / static_cast<double>(up));
        // **The cast is only defined for a value the type can hold**, and every
        // input to the estimate is the caller's: an attenuation of 1e18, or a
        // bandwidth a hair under one, sends it past 2^64. It used to be the
        // 40..200 dB and 0.5..0.999 windows on those settings that kept this
        // in range, and settings are not where that job belongs. Saturating is
        // right rather than merely safe -- anything this large fails the
        // max_taps gate below, which refuses it in words about the filter.
        constexpr double k_countable = 9.0e18;
        chosen = !(estimate >= 0.0)      ? 0
                 : estimate > k_countable ? static_cast<std::uint64_t>(k_countable)
                                          : static_cast<std::uint64_t>(estimate);
    }
    chosen = std::max<std::uint64_t>(chosen, 8);
    chosen += chosen & 1u; // even, so the prototype's centre lands on a sample

    // Kaiser's formula is an estimate, and an estimate lands a few tenths of a
    // decibel short about as often as it lands over. `verify` turns the
    // specification into a promise: build it, measure it, and buy the shortfall
    // in taps. Only where the caller left the length open -- somebody who wrote
    // `taps=128` meant 128 -- and not for Parks-McClellan, which is exact at
    // the length it was given and has nothing to correct.
    const bool may_grow = design.taps == 0;
    double expected_stop_db = 0.0; // what Parks-McClellan said it would be

    for (int attempt = 0;; ++attempt) {
        // Saturating, for the same reason: `taps` is now whatever was asked for
        // and `up` is whatever the two rates reduce to, and the product of two
        // numbers this program did not choose has to be allowed to be too big
        // to count rather than allowed to wrap.
        const std::uint64_t length =
            chosen > (std::numeric_limits<std::uint64_t>::max() - 1) / up
                ? std::numeric_limits<std::uint64_t>::max()
                : chosen * up + 1;
        if (length > design.max_taps) {
            why = "the ratio reduces to " + std::to_string(up) + "/" +
                  std::to_string(down) + ", which needs " + std::to_string(length) +
                  " coefficients. This is a rational resampler and that is more than "
                  "it will build; pick rates with a common factor, or raise max_taps";
            return false;
        }
        taps = static_cast<std::uint32_t>(chosen);

        const auto size = static_cast<std::size_t>(length);
        const std::size_t centre = size / 2;

        if (design.method == Method::remez) {
            // The limit is the interpolation at the heart of the exchange, not
            // the memory. Past about a thousand nodes it loses conditioning in
            // double precision, and a filter that came out of a diverged
            // exchange looks like a filter.
            const std::size_t k_remez_limit = design.remez_max_taps;
            if (size > k_remez_limit) {
                why = "Parks-McClellan is exact and this prototype is " +
                      std::to_string(size) + " taps; past " +
                      std::to_string(k_remez_limit) +
                      " the exchange loses conditioning in double precision. The ratio " +
                      std::to_string(up) + "/" + std::to_string(down) +
                      " needs that many because of its numerator. Use design=window, "
                      "which is closed-form at any length, or design=refine";
                return false;
            }
            const double ripple_pass =
                design.passband_ripple_db > 0.0
                    ? std::pow(10.0, design.passband_ripple_db / 20.0) - 1.0
                    : std::pow(10.0, -design.attenuation_db / 20.0);
            const double ripple_stop = std::pow(10.0, -design.attenuation_db / 20.0);
            double deviation = 0.0;
            if (!remez_lowpass(size, passband_edge, stopband_edge, 1.0 / ripple_pass,
                               1.0 / ripple_stop, out, deviation, why)) {
                return false;
            }
            // The exchange's own answer for where the stopband will land. It is
            // checked against the measured response below: an exchange that
            // converged and a recovery that then built a different filter are
            // two failures that look identical from outside.
            expected_stop_db = 20.0 * std::log10(std::abs(deviation) * ripple_stop);
            for (double& tap : out) {
                tap *= gain;
            }
        } else {
            std::vector<double> w;
            switch (design.window) {
            case Window::dolph:
                w = dolph_window(size, design.attenuation_db);
                break;
            case Window::dpss:
                // Kaiser's beta is pi times the time-bandwidth product the
                // Slepian window is defined by, which is exactly the sense in
                // which one approximates the other.
                w = dpss_window(size, kaiser_beta(design.attenuation_db) / k_pi);
                break;
            case Window::kaiser:
                // Made tap by tap below, together with the sinc.
                break;
            }
            out.assign(size, 0.0);
            // Each tap is 2 * cutoff * sinc * window, and the sinc is even about
            // the centre, so each value of it serves the tap at n and the tap at
            // its mirror, taken a block at a time (see `Sinc`).
            const Sinc sinc(std::max(up, down), centre);
            std::array<double, 256> ideal{};
            if (design.window == Window::kaiser) {
                // Symmetric about its centre, and so is the arithmetic that
                // makes it: `n - centre` is the same distance on either side,
                // the window is the same bits there (see `kaiser_points`) and so
                // is the sinc. So each tap is computed once and written to both
                // places -- half the Bessel functions -- and no window as long
                // as the prototype is made to be read once and thrown away.
                const double beta = kaiser_beta(design.attenuation_db);
                std::array<double, 256> window{};
                for (std::size_t n = 0; n <= centre; n += window.size()) {
                    const std::size_t count = std::min(window.size(), centre + 1 - n);
                    mp::transform::kaiser_points(size, beta, n, count, window.data());
                    sinc.down(centre - n, count, ideal.data());
                    for (std::size_t i = 0; i < count; ++i) {
                        ideal[i] = 2.0 * cutoff * ideal[i] * window[i];
                    }
                    mirror(ideal.data(), count, out.data() + n, out.data() + (size - n - count));
                }
            } else {
                // Dolph's window is symmetric by construction, but the Slepian
                // one comes out of an iteration and is only as symmetric as
                // that leaves it, so each tap takes its own point of it.
                for (std::size_t n = 0; n <= centre; n += ideal.size()) {
                    const std::size_t count = std::min(ideal.size(), centre + 1 - n);
                    sinc.down(centre - n, count, ideal.data());
                    const double* low = w.data() + n;
                    const double* high = w.data() + (size - n - count);
                    double* to_low = out.data() + n;
                    double* to_high = out.data() + (size - n - count);
                    for (std::size_t i = 0; i < count; ++i) {
                        to_low[i] = 2.0 * cutoff * ideal[i] * low[i];
                    }
                    // Tap size - 1 - n - i is the mirror of tap n + i, and
                    // takes the same sinc and its own point of the window.
                    for (std::size_t i = 0; i < count; ++i) {
                        to_high[i] = 2.0 * cutoff * ideal[count - 1 - i] * high[i];
                    }
                }
            }
            // Unity gain at DC, scaled by the interpolation factor. The whole
            // prototype is scaled rather than each phase separately:
            // normalising the phases one at a time would flatten DC by bending
            // the response that was just designed.
            const double sum = mp::transform::sum(out.data(), out.size());
            if (sum != 0.0) {
                const double scale = gain / sum;
                for (double& tap : out) {
                    tap *= scale;
                }
            }
        }

        if (design.method == Method::refine) {
            // Two FFTs a round, so the transform is what bounds this rather
            // than the filter. 2^20 points over a 2^17-tap prototype is eight
            // samples per ripple, which is enough to find the peaks that
            // matter.
            constexpr std::size_t k_refine_limit = 1u << 17;
            if (size > k_refine_limit) {
                why = "refining works on the response, which means transforming it, and " +
                      std::to_string(size) +
                      " taps is past the point where that is worth the wait. Use "
                      "design=window";
                return false;
            }
            // Eight points per tap. Four was tried and is measurably worse:
            // a clip decided on a coarse grid moves the peaks it cannot see,
            // and the method stops improving about where the window it started
            // from left off.
            const std::size_t transform =
                std::min<std::size_t>(next_power_of_two(size * 8), std::size_t{1} << 20);
            const double target_pass =
                design.passband_ripple_db > 0.0
                    ? std::pow(10.0, design.passband_ripple_db / 20.0) - 1.0
                    : std::pow(10.0, -design.attenuation_db / 20.0);

            // Every round transforms and measures a filter of the same length,
            // so the plans, the rotations and the room for them are made once.
            Meter meter(size, design.measure_points);
            const Response start = meter(out, passband_edge, stopband_edge, gain);
            // Never worse in the passband than the window design it started
            // from, and never worse than what was asked for. At a length where
            // the specification cannot be met at all, the first of those is the
            // one that binds -- and it should.
            const double ripple_limit = std::max(
                design.passband_ripple_db > 0.0 ? design.passband_ripple_db : 0.01,
                start.passband_ripple_db);

            Response best = start;
            std::vector<double> keep = out;
            mp::transform::RealFft plan(transform);
            const std::vector<std::complex<double>> rotate = rotations(size, transform);
            std::vector<double> signal;
            std::vector<std::complex<double>> spectrum;
            // Alternating projection is not monotone: a round that does not
            // improve is not the end of the improving, and stopping at the
            // first one leaves most of the gain on the table. Patience is what
            // that costs -- eight fruitless rounds before believing it.
            int patience = 0;
            for (int round = 0; round < static_cast<int>(design.refine_rounds) &&
                                patience < static_cast<int>(design.refine_patience);
                 ++round) {
                // Ask for a little better than what it has, until it stops
                // delivering. Asking for the specification instead would stop
                // the moment the specification was met, which is the opposite of
                // what somebody choosing this method wants.
                const double target_stop = std::pow(10.0, best.stopband_db / 20.0) * 0.9;
                project(out, plan, rotate, signal, spectrum, passband_edge, stopband_edge, gain,
                        target_pass, target_stop);
                const Response now = meter(out, passband_edge, stopband_edge, gain);
                if (now.stopband_db >= best.stopband_db - 1e-4 ||
                    now.passband_ripple_db > ripple_limit) {
                    ++patience;
                    continue;
                }
                patience = 0;
                best = now;
                keep = out;
            }
            out = keep;
        }

        if (design.phase == Phase::minimum) {
            // The magnitude is settled; this moves the energy to the front of
            // it. The floor is where the logarithm stops looking: twenty
            // decibels under the stopband is far enough to be a null and near
            // enough to still be a number.
            const double floor_db = design.phase_floor_db < 0.0
                                        ? design.phase_floor_db
                                        : -(design.attenuation_db + 20.0);
            to_minimum_phase(out, floor_db, design.cepstrum);
            const double sum = mp::transform::sum(out.data(), out.size());
            if (sum != 0.0) {
                const double scale = gain / sum;
                for (double& tap : out) {
                    tap *= scale;
                }
            }
        }

        achieved = measure(out, passband_edge, stopband_edge, gain, design.measure_points);

        // Parks-McClellan is always checked against *itself*, which is a
        // different question from whether the specification was met. A
        // converged exchange at a length too short for the specification is
        // still the best filter of that length, and refusing it would be
        // refusing the right answer; a filter that does not match the deviation
        // the exchange settled on is a bug in the recovery and must not reach
        // the audio.
        if (design.method == Method::remez &&
            std::abs(achieved.stopband_db - expected_stop_db) > 1.0) {
            why = "the exchange settled on " + std::to_string(expected_stop_db) +
                  " dB and the filter it produced measures " +
                  std::to_string(achieved.stopband_db) +
                  " dB. Those have to agree, and they do not";
            return false;
        }

        if (!design.verify || achieved.stopband_db <= -design.attenuation_db) {
            return true;
        }
        if (!may_grow || attempt >= 7) {
            why = "the design missed its own specification: " +
                  std::to_string(achieved.stopband_db) + " dB in the stopband where " +
                  std::to_string(-design.attenuation_db) + " was asked for";
            return false;
        }
        // Six per cent more filter, which is a decibel or two, and try again.
        chosen += std::max<std::uint64_t>(2, chosen / 16);
        chosen += chosen & 1u;
    }
}

} // namespace mp::resample

// SPDX-License-Identifier: GPL-3.0-or-later

#include "transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

namespace mp::transform {
namespace {

constexpr double k_pi = 3.14159265358979323846;

/// log2 of `n`, which is a power of two.
unsigned log2_of(std::size_t n) noexcept
{
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n) {
        ++bits;
    }
    return bits;
}

/// cos(2 pi k / n) and sin(2 pi k / n) for k < count, each within about an ulp
/// of the true value.
///
/// A coarse table times a fine one, both from `unit`: k is a coarse step plus a
/// fine one, and the product of two accurate numbers is an accurate number --
/// where a twiddle made from the one before it inherits that one's error and
/// adds its own.
void quarter_turn(std::size_t n, std::size_t count, std::vector<double>& cosine,
                  std::vector<double>& sine)
{
    cosine.assign(count, 0.0);
    sine.assign(count, 0.0);
    if (count == 0) {
        return; // a transform of two points or one has no twiddles to make
    }
    std::size_t fine = 1;
    while (fine * fine < count) {
        fine *= 2;
    }
    std::vector<std::complex<double>> step(fine);
    for (std::size_t j = 0; j < fine; ++j) {
        step[j] = unit(j, n);
    }
    for (std::size_t at = 0; at < count; at += fine) {
        const std::complex<double> base = unit(at, n);
        const double c = base.real();
        const double s = base.imag();
        const std::size_t end = std::min(fine, count - at);
        for (std::size_t j = 0; j < end; ++j) {
            cosine[at + j] = c * step[j].real() - s * step[j].imag();
            sine[at + j] = s * step[j].real() + c * step[j].imag();
        }
    }
}

/// exp(-2 pi i m / n) for m = turn * n/4 + k, from the first quarter turn's
/// cos(2 pi k / n) and sin(2 pi k / n): each quarter turn further is a
/// multiplication by -i, which is a swap and a change of sign and costs nothing
/// in precision.
template <unsigned Turn>
void turned(double c, double s, double& re, double& im) noexcept
{
    if constexpr (Turn == 0) {
        re = c;
        im = -s;
    } else if constexpr (Turn == 1) {
        re = -s;
        im = -c;
    } else if constexpr (Turn == 2) {
        re = -c;
        im = s;
    } else {
        re = s;
        im = c;
    }
}

/// exp(-2 pi i m / n) for any m below n, from the first quarter turn (see
/// `turned`), the quarter looked up.
struct Twiddle {
    const double* cosine;
    const double* sine;
    unsigned shift;   ///< log2 of a quarter of n
    std::size_t mask; ///< a quarter of n, less one

    void at(std::size_t m, double& re, double& im) const noexcept
    {
        const double c = cosine[m & mask];
        const double s = sine[m & mask];
        switch ((m >> shift) & 3U) {
        case 0:
            turned<0>(c, s, re, im);
            break;
        case 1:
            turned<1>(c, s, re, im);
            break;
        case 2:
            turned<2>(c, s, re, im);
            break;
        default:
            turned<3>(c, s, re, im);
            break;
        }
    }
};

using Complex = std::complex<double>;

// The passes do their arithmetic on the real and imaginary parts, read with
// real() and imag() and stored as a new value. Written out like that, a pass is
// a loop the compiler vectorises -- the parts are the two doubles a
// std::complex<double> is laid out as -- where with std::complex's own
// multiplication, one C library's carries a branch for the infinities of Annex
// G, and that loop stays scalar.
//
// A pass that goes from one buffer to the other says so, `__restrict` on both:
// they are the plan's two buffers and never the same. Unsaid, the compiler has
// to allow that a write may land where a later read is, and it either checks
// the two ranges at run time before every inner loop -- each point of a pass
// with four transforms left -- or, where the reads and the writes step at
// different rates, as in the first pass, gives the vector loop up.

/// The butterfly of four points before its twiddles: a + b + c + d, then
/// (a - c) - i(b - d), a - b + c - d and (a - c) + i(b - d), the signs of i as a
/// forward transform has them and the other way round for an inverse. Every
/// pass is this, and all but the last turn three of the four by a twiddle.
template <bool Inverse>
std::array<Complex, 4> sums4(Complex a, Complex b, Complex c, Complex d) noexcept
{
    const double apcr = a.real() + c.real();
    const double apci = a.imag() + c.imag();
    const double amcr = a.real() - c.real();
    const double amci = a.imag() - c.imag();
    const double bpdr = b.real() + d.real();
    const double bpdi = b.imag() + d.imag();
    const double bmdr = b.real() - d.real();
    const double bmdi = b.imag() - d.imag();
    const double t1r = Inverse ? amcr - bmdi : amcr + bmdi;
    const double t1i = Inverse ? amci + bmdr : amci - bmdr;
    const double t3r = Inverse ? amcr + bmdi : amcr - bmdi;
    const double t3i = Inverse ? amci - bmdr : amci + bmdr;
    return {Complex{apcr + bpdr, apci + bpdi}, Complex{t1r, t1i},
            Complex{apcr - bpdr, apci - bpdi}, Complex{t3r, t3i}};
}

/// `v` times the twiddle (re, im).
Complex turn(Complex v, double re, double im) noexcept
{
    return {v.real() * re - v.imag() * im, v.real() * im + v.imag() * re};
}

/// The three twiddles of a butterfly, conjugated for an inverse transform.
struct Twiddles3 {
    double w1r = 0.0;
    double w1i = 0.0;
    double w2r = 0.0;
    double w2i = 0.0;
    double w3r = 0.0;
    double w3i = 0.0;

    template <bool Inverse>
    [[nodiscard]] std::array<Complex, 4> apply(const std::array<Complex, 4>& t) const noexcept
    {
        const double sign = Inverse ? -1.0 : 1.0;
        return {t[0], turn(t[1], w1r, sign * w1i), turn(t[2], w2r, sign * w2i),
                turn(t[3], w3r, sign * w3i)};
    }
};

/// One pass of four points, from `src` to `dst`: `len` is the length of the
/// transforms still to be done and `s` how many of them there are, interleaved,
/// and `w` holds the twiddles of a transform of `len` points. Point `p` of
/// transform `q` is at `q + s * p`, and each of the four quarters of every
/// transform becomes one of four transforms of a quarter of the length, written
/// where the next pass reads them. The loop over the transforms is the one the
/// compiler vectorises, so this is for the passes where there are several; the
/// first, where there is one, is `first4`.
template <bool Inverse>
void pass4(const Complex* __restrict src, Complex* __restrict dst, std::size_t len,
           std::size_t s, const Twiddle& w) noexcept
{
    const std::size_t quarter = len / 4;
    for (std::size_t p = 0; p < quarter; ++p) {
        Twiddles3 t;
        w.at(p, t.w1r, t.w1i);
        w.at(2 * p, t.w2r, t.w2i);
        w.at(3 * p, t.w3r, t.w3i);
        const Complex* x0 = src + s * p;
        const Complex* x1 = x0 + s * quarter;
        const Complex* x2 = x1 + s * quarter;
        const Complex* x3 = x2 + s * quarter;
        Complex* y0 = dst + s * 4 * p;
        Complex* y1 = y0 + s;
        Complex* y2 = y1 + s;
        Complex* y3 = y2 + s;
        for (std::size_t q = 0; q < s; ++q) {
            const std::array<Complex, 4> y =
                t.apply<Inverse>(sums4<Inverse>(x0[q], x1[q], x2[q], x3[q]));
            y0[q] = y[0];
            y1[q] = y[1];
            y2[q] = y[2];
            y3[q] = y[3];
        }
    }
}

/// Points `from` to `to` of the first pass of four points, where there is one
/// transform, the whole of `n = 4 * quarter` points: the loop over the points
/// is the one to vectorise, and each point has twiddles of its own.
///
/// Point p's are exp(-2 pi i m / n) for m = p, 2p and 3p, and the quarter turn
/// each of those is in -- `Turn2` and `Turn3`; p itself is always in the first
/// -- is the same over the whole of the range given, which is what keeps the
/// loop free of branches: `first_pass` cuts the points into the four ranges
/// where that holds.
template <bool Inverse, unsigned Turn2, unsigned Turn3>
void first4(const Complex* __restrict src, Complex* __restrict dst, std::size_t quarter,
            const double* cosine, const double* sine, std::size_t from, std::size_t to) noexcept
{
    // Everything the loop reads and writes is a pointer to where the range
    // starts and a whole multiple of the count from it. With the quarter turns
    // subtracted inside the loop, 2p - quarter is an unsigned difference that
    // the compiler cannot rule out wrapping, and it kept the loop scalar.
    const Complex* x0 = src + from;
    const Complex* x1 = x0 + quarter;
    const Complex* x2 = x1 + quarter;
    const Complex* x3 = x2 + quarter;
    Complex* y = dst + (4 * from);
    const double* c1 = cosine + from;
    const double* s1 = sine + from;
    const double* c2 = cosine + ((2 * from) - (Turn2 * quarter));
    const double* s2 = sine + ((2 * from) - (Turn2 * quarter));
    const double* c3 = cosine + ((3 * from) - (Turn3 * quarter));
    const double* s3 = sine + ((3 * from) - (Turn3 * quarter));
    const std::size_t count = to - from;
    for (std::size_t i = 0; i < count; ++i) {
        Twiddles3 t;
        turned<0>(c1[i], s1[i], t.w1r, t.w1i);
        turned<Turn2>(c2[2 * i], s2[2 * i], t.w2r, t.w2i);
        turned<Turn3>(c3[3 * i], s3[3 * i], t.w3r, t.w3i);
        const std::array<Complex, 4> v =
            t.apply<Inverse>(sums4<Inverse>(x0[i], x1[i], x2[i], x3[i]));
        y[4 * i] = v[0];
        y[(4 * i) + 1] = v[1];
        y[(4 * i) + 2] = v[2];
        y[(4 * i) + 3] = v[3];
    }
}

/// The first pass of four points of a transform of `4 * quarter` points, cut
/// into the ranges of points over which 2p and 3p stay in one quarter turn
/// each (see `first4`).
template <bool Inverse>
void first_pass(const Complex* src, Complex* dst, std::size_t quarter, const double* cosine,
                const double* sine) noexcept
{
    const std::size_t third = (quarter + 2) / 3;           // first p with 3p >= quarter
    const std::size_t half = quarter / 2;                  // first p with 2p >= quarter
    const std::size_t two_thirds = (2 * quarter + 2) / 3;  // first p with 3p >= 2 quarter
    first4<Inverse, 0, 0>(src, dst, quarter, cosine, sine, 0, third);
    first4<Inverse, 0, 1>(src, dst, quarter, cosine, sine, third, half);
    first4<Inverse, 1, 1>(src, dst, quarter, cosine, sine, half, two_thirds);
    first4<Inverse, 1, 2>(src, dst, quarter, cosine, sine, two_thirds, quarter);
}

/// The butterfly of the last pass of four points, whose twiddles are all one,
/// scaled by `scale` -- one for a forward transform, and 1/n, a power of two
/// and so exact, for an inverse.
template <bool Inverse>
std::array<Complex, 4> last4_point(Complex a, Complex b, Complex c, Complex d,
                                   double scale) noexcept
{
    const std::array<Complex, 4> t = sums4<Inverse>(a, b, c, d);
    return {Complex{t[0].real() * scale, t[0].imag() * scale},
            Complex{t[1].real() * scale, t[1].imag() * scale},
            Complex{t[2].real() * scale, t[2].imag() * scale},
            Complex{t[3].real() * scale, t[3].imag() * scale}};
}

/// The last pass of four points, from `src` into `dst`, rows of `s` values.
///
/// This and `last4_here` are one pass written twice, because the compiler makes
/// vector code of each only when it can see which it is: here two buffers,
/// which it checks do not overlap before it takes the vector loop; there one,
/// read and written at the same place, which is safe as it stands. One loop
/// given the same buffer twice fails that check and runs the scalar loop.
template <bool Inverse>
void last4_apart(const Complex* __restrict src, Complex* __restrict dst, std::size_t s,
                 double scale) noexcept
{
    const Complex* x1 = src + s;
    const Complex* x2 = src + (2 * s);
    const Complex* x3 = src + (3 * s);
    Complex* y1 = dst + s;
    Complex* y2 = dst + (2 * s);
    Complex* y3 = dst + (3 * s);
    for (std::size_t q = 0; q < s; ++q) {
        const std::array<Complex, 4> v =
            last4_point<Inverse>(src[q], x1[q], x2[q], x3[q], scale);
        dst[q] = v[0];
        y1[q] = v[1];
        y2[q] = v[2];
        y3[q] = v[3];
    }
}

/// The last pass of four points in place (see `last4_apart`).
template <bool Inverse>
void last4_here(Complex* a, std::size_t s, double scale) noexcept
{
    Complex* r1 = a + s;
    Complex* r2 = a + (2 * s);
    Complex* r3 = a + (3 * s);
    for (std::size_t q = 0; q < s; ++q) {
        const std::array<Complex, 4> v = last4_point<Inverse>(a[q], r1[q], r2[q], r3[q], scale);
        a[q] = v[0];
        r1[q] = v[1];
        r2[q] = v[2];
        r3[q] = v[3];
    }
}

/// The butterfly of a last pass of two points.
std::array<Complex, 2> last2_point(Complex a, Complex b, double scale) noexcept
{
    return {Complex{(a.real() + b.real()) * scale, (a.imag() + b.imag()) * scale},
            Complex{(a.real() - b.real()) * scale, (a.imag() - b.imag()) * scale}};
}

/// The last pass of two points, for a length that is an odd power of two, from
/// `src` into `dst` (see `last4_apart`).
void last2_apart(const Complex* __restrict src, Complex* __restrict dst, std::size_t s,
                 double scale) noexcept
{
    const Complex* x1 = src + s;
    Complex* y1 = dst + s;
    for (std::size_t q = 0; q < s; ++q) {
        const std::array<Complex, 2> v = last2_point(src[q], x1[q], scale);
        dst[q] = v[0];
        y1[q] = v[1];
    }
}

/// The last pass of two points in place (see `last4_apart`).
void last2_here(Complex* a, std::size_t s, double scale) noexcept
{
    Complex* r1 = a + s;
    for (std::size_t q = 0; q < s; ++q) {
        const std::array<Complex, 2> v = last2_point(a[q], r1[q], scale);
        a[q] = v[0];
        r1[q] = v[1];
    }
}

/// How many of the Kaiser window's points are made together (see `kaiser_block`).
constexpr std::size_t k_block = 256;

/// The Kaiser window's values at `first` and the points after it, `count` of
/// them and at most `k_block`, into `out`, for a window of `2 * half + 1` points
/// (or `2 * half + 1` rounded, for an even length, where `half` ends in a half).
///
/// **Each value is the same bits `bessel_i0` gives it alone.** The series are
/// summed side by side, a term of each in turn, and each stops where the scalar
/// loop would have stopped it: a point that is done keeps what it had while the
/// others go on, and neighbours are done within a term or two of each other.
/// What is gained is that the divisions, one a term and most of what a series
/// costs, no longer wait for each other: the loop over the points is a loop the
/// compiler makes vector divisions of.
///
/// **The window is symmetric, and so is this arithmetic.** `n - half` is the same
/// distance on either side of the centre, exactly -- an integer, or an integer
/// and a half, well inside what a double holds exactly -- so the value at `n` and
/// the value at its mirror are the same bits, and a caller that wants both
/// computes one.
void kaiser_block(std::size_t first, std::size_t count, double half, double beta,
                  double denominator, double* out) noexcept
{
    std::array<double, k_block> x{};
    std::array<double, k_block> term{};
    std::array<double, k_block> sum{};
    for (std::size_t i = 0; i < count; ++i) {
        const double ratio = (static_cast<double>(first + i) - half) / half;
        x[i] = beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio));
        term[i] = 1.0;
        sum[i] = 1.0;
    }
    // bessel_i0, point by point.
    for (int k = 1; k < 200; ++k) {
        std::size_t running = 0;
        for (std::size_t i = 0; i < count; ++i) {
            // Stopped, as the scalar loop's `break` stops it, once the last term
            // it added was small enough.
            const bool live = !(term[i] < sum[i] * 1e-18);
            const double step = x[i] / (2.0 * k);
            const double t = term[i] * (step * step);
            const double s = sum[i] + t;
            term[i] = live ? t : term[i];
            sum[i] = live ? s : sum[i];
            running += live && !(t < s * 1e-18) ? 1 : 0;
        }
        if (running == 0) {
            break;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = sum[i] / denominator;
    }
}

} // namespace

std::size_t next_power_of_two(std::size_t n) noexcept
{
    std::size_t k = 1;
    while (k < n) {
        k <<= 1;
    }
    return k;
}

std::complex<double> unit(std::size_t m, std::size_t n) noexcept
{
    const std::size_t quarter = n / 4;
    const std::size_t turn = m / quarter;
    std::size_t rest = m % quarter;
    const bool exchange = rest > quarter / 2;
    if (exchange) {
        rest = quarter - rest;
    }
    // 2 pi / n is exact, n being a power of two.
    const double angle = (2.0 * k_pi / static_cast<double>(n)) * static_cast<double>(rest);
    double c = std::cos(angle);
    double s = std::sin(angle);
    if (exchange) {
        std::swap(c, s);
    }
    switch (turn & 3U) {
    case 0:
        return {c, s};
    case 1:
        return {-s, c};
    case 2:
        return {-c, -s};
    default:
        return {s, -c};
    }
}

// --------------------------------------------------------------------------
// Complex
// --------------------------------------------------------------------------

Fft::Fft(std::size_t n) : n_(n)
{
    // Only the passes before the last have twiddles other than one, and a
    // transform of four points or fewer is its last pass. Each pass has a table
    // of its own: a pass with `len` points of each transform left wants
    // exp(-2 pi i m / len), the twiddles of a transform of that length, which
    // are every (n / len)th of the first pass's. Read from the first pass's
    // table they would cost a cache line apiece; copied out, they are
    // neighbours, and a quarter as many again as the first table in all.
    if (n >= 8) {
        std::vector<double> cosine;
        std::vector<double> sine;
        quarter_turn(n, n / 4, cosine, sine);
        std::size_t total = 0;
        for (std::size_t len = n; len > 4; len /= 4) {
            total += len / 4;
        }
        cos_.reserve(total);
        sin_.reserve(total);
        for (std::size_t len = n; len > 4; len /= 4) {
            const std::size_t step = n / len;
            for (std::size_t j = 0; j < len / 4; ++j) {
                cos_.push_back(cosine[j * step]);
                sin_.push_back(sine[j * step]);
            }
        }
    }
    work_.assign(n, {0.0, 0.0});
}

void Fft::forward(std::complex<double>* a) noexcept
{
    run<false>(a);
}

void Fft::inverse(std::complex<double>* a) noexcept
{
    run<true>(a);
}

template <bool Inverse>
void Fft::run(std::complex<double>* a) noexcept
{
    const std::size_t n = n_;
    if (n < 2) {
        return;
    }
    Complex* data = a;
    Complex* work = work_.data();
    const double scale = Inverse ? 1.0 / static_cast<double>(n) : 1.0;

    // Every pass but the last goes from one buffer to the other.
    Complex* from = data;
    Complex* to = work;
    std::size_t len = n;
    std::size_t s = 1;
    std::size_t table = 0;
    while (len > 4) {
        const std::size_t quarter = len / 4;
        if (s == 1) {
            first_pass<Inverse>(from, to, quarter, cos_.data(), sin_.data());
        } else {
            const Twiddle w{.cosine = cos_.data() + table,
                            .sine = sin_.data() + table,
                            .shift = log2_of(quarter),
                            .mask = quarter - 1};
            pass4<Inverse>(from, to, len, s, w);
        }
        std::swap(from, to);
        table += quarter;
        len = quarter;
        s *= 4;
    }

    // The last pass needs no twiddles and ends where the transform started:
    // from the other buffer, or in place when the passes before it came back
    // here.
    if (len == 4) {
        if (from == data) {
            last4_here<Inverse>(data, s, scale);
        } else {
            last4_apart<Inverse>(from, data, s, scale);
        }
    } else if (from == data) {
        last2_here(data, s, scale);
    } else {
        last2_apart(from, data, s, scale);
    }
}

void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    if (a.size() < 2) {
        return;
    }
    Fft plan(a.size());
    if (inverse) {
        plan.inverse(a.data());
    } else {
        plan.forward(a.data());
    }
}

// --------------------------------------------------------------------------
// Real
// --------------------------------------------------------------------------

RealFft::RealFft(std::size_t n) : n_(n), half_(n / 2)
{
    quarter_turn(n, n / 4, cos_, sin_);
    work_.assign(n / 2, {0.0, 0.0});
}

void RealFft::forward(const double* x, std::complex<double>* bins) noexcept
{
    if (n_ < 2) {
        if (n_ == 1) {
            bins[0] = {x[0], 0.0};
        }
        return;
    }
    const std::size_t m = n_ / 2;
    const std::size_t quarter = n_ / 4;
    // The tables read through pointers of their own: read as members, every
    // store to the spectrum might have changed where they are, as far as the
    // compiler can tell, and the loops stayed scalar.
    const double* cosine = cos_.data();
    const double* sine = sin_.data();
    // The even samples as the real parts and the odd ones as the imaginary
    // parts.
    Complex* z = work_.data();
    for (std::size_t t = 0; t < m; ++t) {
        z[t] = {x[2 * t], x[(2 * t) + 1]};
    }
    half_.forward(z);

    bins[0] = {z[0].real() + z[0].imag(), 0.0};
    bins[m] = {z[0].real() - z[0].imag(), 0.0};
    // Bin k is E + W^k O, where E = (Z[k] + conj Z[m-k]) / 2 is the transform of
    // the even samples and O = (Z[k] - conj Z[m-k]) / 2i that of the odd ones.
    // W^k is (cos, -sin) of the table for the first quarter turn and a quarter
    // turn on from it for the second, which is the loop split in two.
    const auto bin = [&](std::size_t k, double wr, double wi) {
        const double zr = z[k].real();
        const double zi = z[k].imag();
        const double cr = z[m - k].real();
        const double ci = -z[m - k].imag();
        const double er = 0.5 * (zr + cr);
        const double ei = 0.5 * (zi + ci);
        const double orr = 0.5 * (zi - ci);
        const double oi = -0.5 * (zr - cr);
        bins[k] = {er + (wr * orr - wi * oi), ei + (wr * oi + wi * orr)};
    };
    for (std::size_t k = 1; k < std::min(quarter, m); ++k) {
        bin(k, cosine[k], -sine[k]);
    }
    for (std::size_t k = std::max<std::size_t>(quarter, 1); k < m; ++k) {
        bin(k, -sine[k - quarter], -cosine[k - quarter]);
    }
}

void RealFft::inverse(const std::complex<double>* bins, double* x) noexcept
{
    if (n_ < 2) {
        if (n_ == 1) {
            x[0] = bins[0].real();
        }
        return;
    }
    const std::size_t m = n_ / 2;
    const std::size_t quarter = n_ / 4;
    // Undoing `forward`: E and O from bins k and m-k, and Z = E + iO, whose
    // inverse transform of half the length is the even samples and the odd
    // ones. O is (X[k] - conj X[m-k]) conj(W^k) / 2.
    const double* cosine = cos_.data();
    const double* sine = sin_.data();
    Complex* z = work_.data();
    const double x0 = bins[0].real();
    const double xm = bins[m].real();
    z[0] = {0.5 * (x0 + xm), 0.5 * (x0 - xm)};
    const auto bin = [&](std::size_t k, double wr, double wi) {
        const double ar = bins[k].real();
        const double ai = bins[k].imag();
        const double br = bins[m - k].real();
        const double bi = -bins[m - k].imag();
        const double er = 0.5 * (ar + br);
        const double ei = 0.5 * (ai + bi);
        const double dr = 0.5 * (ar - br);
        const double di = 0.5 * (ai - bi);
        const double orr = dr * wr + di * wi;
        const double oi = di * wr - dr * wi;
        z[k] = {er - oi, ei + orr};
    };
    for (std::size_t k = 1; k < std::min(quarter, m); ++k) {
        bin(k, cosine[k], -sine[k]);
    }
    for (std::size_t k = std::max<std::size_t>(quarter, 1); k < m; ++k) {
        bin(k, -sine[k - quarter], -cosine[k - quarter]);
    }
    half_.inverse(z);
    for (std::size_t t = 0; t < m; ++t) {
        x[2 * t] = z[t].real();
        x[(2 * t) + 1] = z[t].imag();
    }
}

// --------------------------------------------------------------------------
// Any length
// --------------------------------------------------------------------------

void dft_any(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    if (n < 2) {
        return;
    }
    if ((n & (n - 1)) == 0) {
        fft(a, false);
        return;
    }

    // Bluestein: n*k = (n^2 + k^2 - (k-n)^2) / 2 turns a transform of any length
    // into a convolution, which a power-of-two FFT can do.
    const std::size_t m = next_power_of_two(2 * n - 1);
    std::vector<std::complex<double>> x(m, {0.0, 0.0});
    std::vector<std::complex<double>> y(m, {0.0, 0.0});

    const auto chirp = [n](std::size_t i) {
        // fmod keeps the argument small: i*i overflows the exactly-representable
        // range of a double long before n does.
        const double phase =
            k_pi * std::fmod(static_cast<double>(i) * static_cast<double>(i),
                             2.0 * static_cast<double>(n)) /
            static_cast<double>(n);
        return std::complex<double>{std::cos(phase), std::sin(phase)};
    };

    // Each chirp once: four uses of every one of them, and each is a cosine and
    // a sine.
    std::vector<std::complex<double>> c(n);
    for (std::size_t i = 0; i < n; ++i) {
        c[i] = chirp(i);
    }
    // The products written out (see `turn`), each loop over neighbours, and
    // each value built from its parts: a std::complex copied as a whole is a
    // 16-byte copy of memory to the compiler, which it does not vectorise.
    Complex* xs = x.data();
    Complex* ys = y.data();
    const Complex* cs = c.data();
    Complex* as = a.data();
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = turn(as[i], cs[i].real(), -cs[i].imag());
        ys[i] = {cs[i].real(), cs[i].imag()};
    }
    for (std::size_t i = 1; i < n; ++i) {
        ys[m - i] = {cs[i].real(), cs[i].imag()};
    }
    // One plan for the three transforms.
    Fft plan(m);
    plan.forward(xs);
    plan.forward(ys);
    for (std::size_t i = 0; i < m; ++i) {
        xs[i] = turn(xs[i], ys[i].real(), ys[i].imag());
    }
    plan.inverse(xs);
    for (std::size_t i = 0; i < n; ++i) {
        as[i] = turn(xs[i], cs[i].real(), -cs[i].imag());
    }
}

// --------------------------------------------------------------------------
// The Kaiser window
// --------------------------------------------------------------------------

double kaiser_beta(double attenuation_db) noexcept
{
    if (attenuation_db > 50.0) {
        return 0.1102 * (attenuation_db - 8.7);
    }
    if (attenuation_db >= 21.0) {
        return 0.5842 * std::pow(attenuation_db - 21.0, 0.4) +
               0.07886 * (attenuation_db - 21.0);
    }
    return 0.0;
}

double bessel_i0(double x) noexcept
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18) {
            break;
        }
    }
    return sum;
}

void kaiser_points(std::size_t length, double beta, std::size_t first, std::size_t count,
                   double* out) noexcept
{
    if (length < 2) {
        std::fill_n(out, count, 1.0);
        return;
    }
    const double half = static_cast<double>(length - 1) / 2.0;
    const double denominator = bessel_i0(beta);
    for (std::size_t done = 0; done < count; done += k_block) {
        kaiser_block(first + done, std::min(k_block, count - done), half, beta, denominator,
                     out + done);
    }
}

std::vector<double> kaiser_window(std::size_t length, double beta)
{
    std::vector<double> w(length, 1.0);
    // Half of it, mirrored: the Bessel function is the whole cost, and the
    // other half would be the same values again (see `kaiser_block`).
    kaiser_points(length, beta, 0, (length + 1) / 2, w.data());
    for (std::size_t n = 0; n < length / 2; ++n) {
        w[length - 1 - n] = w[n];
    }
    return w;
}

// --------------------------------------------------------------------------
// Minimum phase
// --------------------------------------------------------------------------

void to_minimum_phase(std::vector<double>& h, double floor_db, unsigned oversample)
{
    const std::size_t length = h.size();
    if (length < 2) {
        return;
    }
    // Room for the cepstrum to decay in. Too little and it wraps, which shows
    // up as a filter whose magnitude is not the one it was given. The cap keeps
    // the room in memory; it never makes the room shorter than the filter,
    // which would not hold it at all.
    const std::size_t factor = std::max<std::size_t>(oversample, 2);
    const std::size_t n =
        std::max(std::min<std::size_t>(next_power_of_two(length * factor), std::size_t{1} << 22U),
                 next_power_of_two(length));

    // Every sequence here is real -- the filter, its cepstrum, the fold of it,
    // and the filter that comes back -- so every transform is a real one: half
    // the length, and half the bins to take a logarithm or an exponential of.
    RealFft transform(n);
    std::vector<double> signal(n, 0.0);
    std::copy_n(h.begin(), length, signal.begin());
    std::vector<std::complex<double>> spectrum(n / 2 + 1);
    transform.forward(signal.data(), spectrum.data());

    // The magnitudes, kept where the spectrum was: the phase is not needed
    // again, and a magnitude is a square root of a sum of squares, taken with
    // care for overflow -- once for each bin is enough.
    double peak = 0.0;
    for (auto& value : spectrum) {
        const double magnitude = std::abs(value);
        value = {magnitude, 0.0};
        peak = std::max(peak, magnitude);
    }
    const double floor = peak * std::pow(10.0, floor_db / 20.0);
    for (auto& value : spectrum) {
        value = {std::log(std::max(value.real(), floor)), 0.0};
    }

    // The real cepstrum, folded onto its causal half. Everything a minimum
    // phase filter is follows from that fold.
    transform.inverse(spectrum.data(), signal.data());
    for (std::size_t k = 1; k < n / 2; ++k) {
        signal[k] *= 2.0;
    }
    std::fill(signal.begin() + static_cast<std::ptrdiff_t>((n / 2) + 1), signal.end(), 0.0);
    transform.forward(signal.data(), spectrum.data());
    for (auto& value : spectrum) {
        value = std::exp(value);
    }
    transform.inverse(spectrum.data(), signal.data());

    std::copy_n(signal.begin(), length, h.begin());
}

} // namespace mp::transform

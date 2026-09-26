// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transforms, and the one thing built on them that more than one module
// wants.
//
// These began inside the resampler, which is where they were first needed. They
// are here because the equaliser needs the same four: a fast transform to
// design a filter with, an any-length one to build a window with, the Kaiser
// window both of them taper with, and the cepstral factorisation that turns a
// magnitude response into a filter whose energy is at the front of it -- and
// the sums all of it is made of. Shared the way `modules/mp4` and
// `modules/biquad` are shared -- by the thing neither owner should own.

#ifndef MEDIAPERCH_TRANSFORM_HPP
#define MEDIAPERCH_TRANSFORM_HPP

#include <complex>
#include <cstddef>
#include <vector>

namespace mp::transform {

/// The sum of `x[0]` to `x[n - 1]`, added in the order that is fastest on the
/// machine the build is for: as many running sums side by side as its vector
/// unit wants -- a few for SSE2, sixteen and more for AVX2 and AVX-512 --
/// joined at the end.
///
/// One running sum waits on its own last addition for every term, and a
/// compiler may not split it: that changes the rounding, and it is allowed only
/// where it is told it may be. Here it is told, for this loop alone. The order
/// of the additions is then the compiler's, and whatever the order, the error is
/// bounded by what one running sum's is bounded by -- in the order it takes, each
/// running sum is a fraction as long, and joining them adds a handful of
/// roundings rather than thousands. Nothing is approximated and no operation
/// changes; only the order of the additions does.
[[nodiscard]] inline double sum(const double* x, std::size_t n) noexcept
{
#pragma clang fp reassociate(on)
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        total += x[i];
    }
    return total;
}

/// The sum of `a[i] * b[i]` for i below `n`, added the same way (see `sum`).
[[nodiscard]] inline double dot(const double* a, const double* b, std::size_t n) noexcept
{
#pragma clang fp reassociate(on)
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        total += a[i] * b[i];
    }
    return total;
}

/// The sum of `term(i)` for i below `n`, added the same way (see `sum`): only
/// the additions are the compiler's to order, not the arithmetic of a term.
template <class Term>
[[nodiscard]] double sum_of(std::size_t n, Term term) noexcept
{
#pragma clang fp reassociate(on)
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        total += term(i);
    }
    return total;
}

[[nodiscard]] std::size_t next_power_of_two(std::size_t n) noexcept;

/// exp(2 pi i m / n) for a whole m below n, n a power of two and at least 4.
///
/// The angle is brought down to the first eighth of a turn with whole numbers
/// first -- a quarter turn is a multiplication by i, and the second eighth of a
/// quarter is the first with the cosine and the sine exchanged -- so what is
/// rounded is a small angle, once, and not one of up to a whole turn.
[[nodiscard]] std::complex<double> unit(std::size_t m, std::size_t n) noexcept;

/// Transforms of one power-of-two length, planned: the twiddles, worked out
/// once, and the room the transform works in.
///
/// Made where allocating is allowed and used where it is not -- `forward` and
/// `inverse` allocate nothing, which is what lets the convolver transform every
/// block it plays. The room is the plan's, so a plan does one transform at a
/// time.
///
/// **Stockham's arrangement, four points at a time.** Each pass reads one
/// buffer and writes the other in the order the next pass wants, so the result
/// comes out in order with no bit-reversal pass -- the one pass of a textbook
/// transform whose reads jump about -- and every read and write is a run of
/// neighbours, which is what lets the compiler make vector code of the inner
/// loop. Four points a pass is half the passes of two, and a quarter fewer
/// multiplications.
///
/// **Each twiddle is computed, not stepped to.** A table made by multiplying
/// one twiddle by a step to get the next loses a little with every step, and a
/// million steps made that five orders of magnitude: 6.5e-11 of the signal at
/// 2^21 points, where this is 1.2e-15.
class Fft {
public:
    /// A plan for nothing, to be assigned one for something.
    Fft() = default;
    /// `n` must be a power of two.
    explicit Fft(std::size_t n);

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    /// In place, `size()` values.
    void forward(std::complex<double>* a) noexcept;
    /// In place, and scaled by 1/n, so that it undoes `forward`.
    void inverse(std::complex<double>* a) noexcept;

private:
    template <bool Inverse>
    void run(std::complex<double>* a) noexcept;

    std::size_t n_ = 0;
    /// The first quarter turn of every pass's twiddles, one pass after another
    /// (see the constructor).
    std::vector<double> cos_;
    std::vector<double> sin_;
    std::vector<std::complex<double>> work_;
};

/// Transforms of real sequences of one power-of-two length, through a complex
/// transform of half that length: the even samples as its real parts and the
/// odd ones as its imaginary parts, pulled apart afterwards. Half the
/// arithmetic of transforming the sequence as a complex one, and half the
/// spectrum to do anything with afterwards -- the other half is its mirror
/// image.
class RealFft {
public:
    /// A plan for nothing, to be assigned one for something.
    RealFft() = default;
    /// `n` must be a power of two.
    explicit RealFft(std::size_t n);

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    /// The spectrum of the `size()` values at `x`: its bins 0 to n/2, which is
    /// `size() / 2 + 1` of them. The others are the conjugates of these.
    void forward(const double* x, std::complex<double>* bins) noexcept;
    /// The `size()` values whose spectrum is `bins` 0 to n/2 and their
    /// conjugates, scaled by 1/n so that it undoes `forward`. A real sequence's
    /// bins 0 and n/2 are real, and their imaginary parts are taken to be zero.
    void inverse(const std::complex<double>* bins, double* x) noexcept;

private:
    std::size_t n_ = 0;
    Fft half_;
    /// exp(-2 pi i k / n) for the first quarter turn, k < n/4.
    std::vector<double> cos_;
    std::vector<double> sin_;
    std::vector<std::complex<double>> work_;
};

/// A power-of-two FFT in place, planned for this one call (see `Fft`).
void fft(std::vector<std::complex<double>>& a, bool inverse);

/// The DFT of any length, via Bluestein's chirp-z. Needed because a window is
/// as long as the filter and filter lengths are 11201 and 25281 rather than
/// 16384.
void dft_any(std::vector<std::complex<double>>& a);

/// Kaiser's beta for a window whose sidelobes are `attenuation_db` down: his
/// formula.
[[nodiscard]] double kaiser_beta(double attenuation_db) noexcept;

/// I0, the modified Bessel function of the first kind and order zero, which a
/// Kaiser window is made of: its series, summed until a term no longer counts.
///
/// Each term is the one before times (x / 2k)^2, a division apiece. x^2 / 4
/// taken once and multiplied by a table of 1/k^2 is quicker, and was measured
/// less accurate -- up to a third more error, from raising the one rounded x^2
/// to every power -- so the division stays, and the speed comes from doing many
/// of them at once instead (see `kaiser_points`).
[[nodiscard]] double bessel_i0(double x) noexcept;

/// Points `first` to `first + count - 1` of the Kaiser window of `length` points
/// and parameter `beta`, into `out`: each the value `kaiser_window` has there,
/// to the bit, for a caller that wants the window a piece at a time.
void kaiser_points(std::size_t length, double beta, std::size_t first, std::size_t count,
                   double* out) noexcept;

/// The Kaiser window of `length` points and parameter `beta`. A window of one
/// point is 1: it has nothing to taper.
[[nodiscard]] std::vector<double> kaiser_window(std::size_t length, double beta);

/// Replaces `h` with the minimum-phase filter of the same magnitude response.
///
/// The real cepstrum, folded onto its causal half: a spectrum's magnitude
/// decides its minimum-phase counterpart, and the fold is how that counterpart
/// is computed. `floor_db` is where the logarithm is clamped -- a stopband null
/// is a true zero, and the logarithm of zero has no folded version.
/// `oversample` is how much room the cepstrum gets, as a multiple of the
/// filter's length: too little and its tail wraps onto its head, which comes
/// out as a filter with the right shape and the wrong magnitude.
void to_minimum_phase(std::vector<double>& h, double floor_db, unsigned oversample);

} // namespace mp::transform

#endif // MEDIAPERCH_TRANSFORM_HPP

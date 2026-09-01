// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dither and noise shaping: what to do about the bits that do not fit.
//
// Rounding alone is not a small error, it is a *correlated* one. The residue of
// a quantiser fed a signal is a function of that signal, so it arrives at the
// ear as distortion -- harmonics that were not in the music -- rather than as
// noise. Dither decorrelates it by making the residue depend on a random value
// instead, which costs about half a bit of noise floor and buys a quantiser
// whose error is white and signal-independent. That trade is why every mastering
// chain in the world takes it.
//
// Noise shaping is the second half. The quantiser's error is unavoidable, but
// *where in the spectrum it lands* is not: feeding it back through a filter
// moves it out of the band the ear is sensitive to and into the band it is not.
// Total noise power goes up; audible noise goes down.
//
// Both run on the decode thread, inside `Converter`. Neither is ever on the
// render thread.

#ifndef MEDIAPERCH_DITHER_HPP
#define MEDIAPERCH_DITHER_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace mp {

/// The probability distribution the dither is drawn from.
///
/// They differ in two things that matter: the shape of the noise floor they
/// leave, and whether that floor *modulates* with the signal. Nothing here is a
/// matter of taste about which is best -- they are different trades and the
/// person listening owns the choice.
enum class DitherKind : std::uint32_t {
    /// None. The error is a function of the signal and arrives as distortion.
    /// Correct for a measurement, wrong for listening, and the only setting
    /// that makes two runs produce identical bytes.
    none,
    /// Rectangular, ±½ LSB. Decorrelates the error but leaves its *variance*
    /// depending on the signal -- audible as a noise floor that breathes with
    /// the music. Almost never the right answer; here because it is the one
    /// everybody reaches for first and the reason not to is worth writing down.
    rectangular,
    /// Triangular, the sum of two rectangular draws. Decorrelates the error and
    /// its variance both, at 4.77 dB more noise than rectangular. **The default,
    /// and the standard answer.**
    triangular,
    /// Triangular made by differencing successive rectangular draws instead of
    /// adding independent ones. Same triangular distribution, and its spectrum
    /// is first-order highpass rather than white -- the noise it adds sits where
    /// the ear is least sensitive at no cost in total power.
    highpass_triangular,
    /// Gaussian, standard deviation ½ LSB. The distribution a *sum* of many
    /// independent noise sources tends to, which makes it the natural choice
    /// when the output feeds another quantiser downstream.
    gaussian,
};

/// The error-feedback filter that decides where the quantisation noise lands.
///
/// The coefficients are the binomial expansion of (1 - z^-1)^N, which is what
/// makes the noise transfer function exactly an Nth-order highpass. They are
/// *derived*, not transcribed: h[k] = (-1)^(k+1) C(N,k).
///
/// **Order is not quality.** Each order tilts the noise further out of the
/// midband, and each order also multiplies the total noise power: a 9th-order
/// binomial shaper puts 48 dB more noise above 15 kHz than it removes below it.
/// Orders 1 to 3 are useful; past that this is the mechanism rather than a
/// recommendation, and a psychoacoustically weighted shaper -- Lipshitz's, or
/// SoX's shibata curves -- is a table of measured coefficients rather than
/// anything a binomial expansion produces. Those belong here too and are not
/// here yet, for the reason `tools/gen_aac_tables.py` exists: a table copied
/// from memory is a table that is wrong.
struct NoiseShaping {
    /// 0 is off. Above `k_max_order` is clamped.
    std::uint32_t order = 0;

    static constexpr std::uint32_t k_max_order = 9;
};

[[nodiscard]] const char* dither_kind_name(DitherKind kind) noexcept;
[[nodiscard]] bool dither_kind_from_name(std::string_view name, DitherKind& out) noexcept;

/// One dither generator and one error-feedback filter, per stream.
///
/// Holds the history the shaper needs, so it is per-channel: a shaper shared
/// between channels feeds each channel's error into the others and correlates
/// the noise across them, which is audible as a centre image on what should be
/// two independent noise floors.
class Dither {
public:
    Dither(DitherKind kind, NoiseShaping shaping, std::uint32_t seed) noexcept;

    /// The value to add before rounding, in LSBs.
    [[nodiscard]] double next() noexcept;

    /// What the feedback filter wants subtracted from this sample, in LSBs.
    /// Zero when shaping is off.
    [[nodiscard]] double feedback() const noexcept;

    /// The error this sample actually made, in LSBs, handed back so the filter
    /// can shape the next one.
    void accept(double error) noexcept;

    void reset() noexcept;

    [[nodiscard]] DitherKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint32_t order() const noexcept { return order_; }

private:
    [[nodiscard]] double uniform() noexcept;

    DitherKind kind_;
    std::uint32_t order_;
    std::uint32_t seed_;
    std::uint32_t rng_;
    /// The previous rectangular draw, for the highpass construction.
    double previous_ = 0.0;
    /// h[k] for k = 1..order, and the errors they multiply.
    std::array<double, NoiseShaping::k_max_order> taps_{};
    std::array<double, NoiseShaping::k_max_order> history_{};
};

} // namespace mp

#endif // MEDIAPERCH_DITHER_HPP

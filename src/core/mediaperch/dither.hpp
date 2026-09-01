// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dither and noise shaping: what to do about the bits that do not fit.
//
// Rounding alone is not a small error, it is a *correlated* one. The residue of
// a quantiser fed a signal is a function of that signal, so it arrives at the
// ear as distortion -- harmonics that were not in the music -- rather than as
// noise. Dither decorrelates it by making the residue depend on a random value
// instead, which costs about half a bit of noise floor and buys a quantiser
// whose error is white and signal-independent. That trade is why every
// mastering chain in the world takes it.
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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

/// Where the quantisation noise is put.
struct NoiseShaping {
    enum class Kind : std::uint32_t {
        /// Left where it fell: white, flat, and as loud in the midband as
        /// anywhere else.
        none,
        /// A noise transfer function of exactly (1 - z^-1)^N. The coefficients
        /// are the binomial expansion, *derived* -- there is nothing to mistype.
        ///
        /// **Order is not quality.** Each order tilts the noise further out of
        /// the midband and each order also multiplies the total: a 9th-order
        /// binomial shaper puts far more noise above 15 kHz than it takes from
        /// below. Orders 1 to 3 are useful; past that this is the mechanism
        /// rather than a recommendation, and the curves below are what somebody
        /// listening should reach for instead.
        binomial,
        /// SSRC's ATH-weighted curves, transcribed by
        /// `tools/gen_shaper_tables.py` from https://github.com/shibatch/ssrc.
        /// Measured against the absolute threshold of hearing rather than
        /// derived, and therefore indexed by sample rate as well as intensity.
        shibata,
    };

    Kind kind = Kind::none;
    /// `binomial`: the order, 1 to `k_max_order`.
    /// `shibata`: SSRC's intensity. Low is gentle; 98 is a plain first-order
    /// shaper and 99 is none.
    std::uint32_t strength = 0;

    static constexpr std::uint32_t k_max_order = 9;
};

[[nodiscard]] const char* dither_kind_name(DitherKind kind) noexcept;
[[nodiscard]] bool dither_kind_from_name(std::string_view name, DitherKind& out) noexcept;

/// `0`..`9` for a binomial order, or `shibata:N` for one of SSRC's curves.
[[nodiscard]] bool noise_shaping_from_name(std::string_view name, NoiseShaping& out) noexcept;
/// What the shaper actually resolved to, for the report a user reads.
[[nodiscard]] std::string noise_shaping_describe(const NoiseShaping& shaping,
                                                 std::uint32_t sample_rate);

/// One dither generator and one error-feedback filter, per channel.
///
/// Per channel because a shaper shared between them feeds each channel's error
/// into the others, correlating what should be two independent noise floors
/// into a centre image.
///
/// The feedback convention is SSRC's, since that is where the curves come from:
/// the filtered history is *added* to the sample before quantising, and the
/// error stored is `quantised - that sum`. Written the other way round the
/// same coefficients shape the noise into the midband rather than out of it.
class Dither {
public:
    /// `sample_rate` chooses among the measured curves and is ignored by the
    /// other kinds. A `shibata` shaper with no curve for that rate falls back
    /// to no shaping rather than to a curve fitted for a different one.
    Dither(DitherKind kind, NoiseShaping shaping, std::uint32_t sample_rate,
           std::uint32_t seed);

    /// The value to add before rounding, in LSBs.
    [[nodiscard]] double next() noexcept;

    /// What the filter says this sample owes for the errors before it, in LSBs.
    [[nodiscard]] double feedback() const noexcept;

    /// The error this sample made, in LSBs. `clipped` bounds what is stored:
    /// an overloaded sample produces an error far larger than an LSB and a
    /// feedback loop handed one rings.
    void accept(double error, bool clipped) noexcept;

    void reset() noexcept;

    [[nodiscard]] DitherKind kind() const noexcept { return kind_; }
    /// How many taps the shaper actually has. 0 when it is off, which includes
    /// a `shibata` setting for which no curve existed.
    [[nodiscard]] std::uint32_t taps() const noexcept
    {
        return static_cast<std::uint32_t>(taps_.size());
    }

private:
    [[nodiscard]] double uniform() noexcept;

    DitherKind kind_;
    std::uint32_t seed_;
    std::uint32_t rng_;
    /// The previous rectangular draw, for the highpass construction.
    double previous_ = 0.0;
    std::vector<double> taps_;
    std::vector<double> history_;
};

} // namespace mp

#endif // MEDIAPERCH_DITHER_HPP

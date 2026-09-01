// SPDX-License-Identifier: GPL-3.0-or-later
//
// Second-order sections, and the cascade of them that an equaliser is.
//
// Shared, because two very different things want the same arithmetic: the
// equaliser is a cascade a person chose, and the loudness meter is a cascade
// ITU-R BS.1770 chose. Neither should own it, the way `modules/mp4` is owned by
// neither the ALAC decoder nor the AAC one.
//
// **The frequency axis is continuous here and stays continuous.** A band is a
// frequency in hertz, a gain in decibels and a Q -- not a slider in a bank of
// thirty-one -- and the composite response is computable at any frequency at
// all, which is what `magnitude_db` is for. That function is the same one a
// display would draw the curve with and the same one the tests measure against,
// so what is drawn and what is heard cannot drift apart.
//
// The coefficient formulas are Robert Bristow-Johnson's, from the audio EQ
// cookbook -- derived from the bilinear transform of each analogue prototype,
// and written out here rather than referenced because they are eight lines and
// a reference that goes missing is a filter nobody can check.

#ifndef MEDIAPERCH_BIQUAD_HPP
#define MEDIAPERCH_BIQUAD_HPP

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::biquad {

enum class Kind : std::uint32_t {
    /// A bump or a dip at `frequency`, `q` wide. The one an equaliser is mostly
    /// made of.
    peak,
    lowshelf,
    highshelf,
    lowpass,
    highpass,
    bandpass,
    notch,
    allpass,
};

/// One band, in the units a person thinks in.
struct Band {
    Kind kind = Kind::peak;
    double frequency_hz = 1000.0;
    /// Peaking and shelving only. The others have no gain to set.
    double gain_db = 0.0;
    /// Bandwidth. For a peak it is f0/bandwidth; for a shelf it sets the slope;
    /// for a pass it sets the resonance, where 0.7071 is Butterworth.
    double q = 0.70710678118654752;
    bool enabled = true;

    friend bool operator==(const Band&, const Band&) = default;
};

/// Normalised coefficients: a0 is divided out, so the difference equation is
/// y = b0 x + b1 x' + b2 x'' - a1 y' - a2 y''.
struct Coefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    /// H(e^{jw}) exactly, for w in radians per sample.
    [[nodiscard]] std::complex<double> response(double omega) const noexcept;
};

[[nodiscard]] bool kind_from_name(const std::string& name, Kind& out);
[[nodiscard]] const char* kind_name(Kind kind) noexcept;
[[nodiscard]] std::string kind_names();

/// `peak:1000:+3:1.0` -- kind, hertz, decibels, Q. The last two may be left off
/// where the kind has no use for them. A leading `-` disables the band without
/// forgetting it, which is what a bypass button is.
[[nodiscard]] bool parse_band(const std::string& text, Band& out, std::string& why);
/// `bands=` in the same words, semicolons between.
[[nodiscard]] bool parse_bands(const std::string& text, std::vector<Band>& out,
                               std::string& why);
[[nodiscard]] std::string band_text(const Band& band);
[[nodiscard]] std::string bands_text(const std::vector<Band>& bands);

/// The bilinear-transformed prototype. Fails when the band cannot exist at this
/// rate -- a 24 kHz band in a 44.1 kHz stream has no analogue below Nyquist,
/// and warping it down to fit would be answering a different question.
[[nodiscard]] bool design(const Band& band, double sample_rate, Coefficients& out,
                          std::string& why);

/// A cascade, and one set of state per channel per section.
///
/// Transposed direct form II: fewer state variables than direct form I and,
/// more to the point, the form whose rounding behaves itself when a section's
/// poles are close to the unit circle -- which is every band below a hundred
/// hertz at a hundred and ninety-two thousand samples a second.
class Cascade {
public:
    [[nodiscard]] bool configure(const std::vector<Band>& bands, double sample_rate,
                                 std::uint32_t channels, std::string& why);
    /// Coefficients straight in, for a filter nobody designed from a Band --
    /// the loudness meter's K-weighting is specified as coefficients.
    void set_sections(const std::vector<Coefficients>& sections, std::uint32_t channels);

    void reset() noexcept;
    /// In place is allowed: `in` and `out` may be the same planes.
    void process(const double* const* in, std::uint32_t frames, double* const* out) noexcept;

    [[nodiscard]] std::size_t sections() const noexcept { return sections_.size(); }
    [[nodiscard]] const Coefficients& section(std::size_t i) const noexcept
    {
        return sections_[i];
    }

    /// The composite response at `hz`. The curve, at any point on it.
    [[nodiscard]] std::complex<double> response(double hz) const noexcept;
    [[nodiscard]] double magnitude_db(double hz) const noexcept;
    [[nodiscard]] double phase_radians(double hz) const noexcept;
    /// The largest magnitude anywhere on the axis, in dB, sampled finely enough
    /// to find it. What a caller needs to know before it clips.
    [[nodiscard]] double peak_gain_db(std::uint32_t points = 4096) const noexcept;

private:
    std::vector<Coefficients> sections_;
    /// Two state variables per section per channel, laid out per channel so one
    /// channel's filtering touches one run of memory.
    std::vector<double> state_;
    std::uint32_t channels_ = 0;
    double sample_rate_ = 0.0;
};

} // namespace mp::biquad

#endif // MEDIAPERCH_BIQUAD_HPP

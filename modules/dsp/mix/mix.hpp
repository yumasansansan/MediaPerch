// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel matrix: which input speaker reaches which output speaker, and how
// much of it.
//
// This is the third geometry a stage can change. The converter changes the
// sample type, the resampler changes the rate, and this changes the channel
// count -- and it is the one with the least defensible answer, because there is
// no arithmetic that makes 5.1 into stereo without deciding what a listener is
// supposed to lose.
//
// **Downmixing is a choice and the choice is visible.** Every coefficient here
// comes from a setting with a documented default, the matrix that was built is
// reported, and the level it was scaled by is reported with it. Nothing is
// derived from a table nobody can see.
//
// **Upmixing does not invent.** Stereo into 5.1 puts the left channel in the
// left speaker, the right in the right, and *silence* everywhere else. Matrix
// upmixing -- deriving a centre and a surround that were never recorded -- is
// speculation of the same kind as guessing the dither that a 16-bit master had
// removed, and design.md refuses that one by name. It is available, because
// somebody will want it and because refusing to implement it would only move it
// somewhere with less scrutiny, but it is off and it is called what it is.

#ifndef MEDIAPERCH_MIX_HPP
#define MEDIAPERCH_MIX_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mp::mix {

/// What to do about a matrix whose rows can add up to more than one.
enum class Normalise : std::uint32_t {
    /// The coefficients as written. The level is whatever it is, and a
    /// correlated signal can reach the quantiser above full scale.
    none,
    /// Scale so that no row's coefficients sum to more than one, which cannot
    /// clip for any input at all. Costs 7.7 dB on a 5.1 downmix, for a case --
    /// every channel simultaneously at full scale and in phase -- that music
    /// does not contain.
    peak,
    /// Scale so that no row's coefficients have more than unit *power*. Keeps
    /// the loudness of uncorrelated content, which is what a downmix normally
    /// faces, and costs 3 dB on the same 5.1.
    energy,
};

/// The coefficients, in dB, that decide a downmix. Defaults are the -3 dB
/// convention: half the power of a shared channel into each side that gets it.
struct Recipe {
    double centre_db = -3.0;
    double surround_db = -3.0;
    /// The low-frequency channel, which is dropped by default. It is not part
    /// of the programme in the way the others are -- it is an effects channel
    /// with its own calibration -- and folding it into a stereo pair at the
    /// level it was mixed at is a well-known way to make a downmix boom.
    double lfe_db = -1000.0;
    Normalise normalise = Normalise::energy;
    /// Allow producing a channel that nothing feeds, by deriving it. Off, such
    /// a channel is silent.
    bool synthesise = false;
};

/// `outputs` by `inputs` coefficients, row-major, plus what normalising did.
struct Matrix {
    std::uint32_t inputs = 0;
    std::uint32_t outputs = 0;
    std::vector<double> coefficients;
    /// One number for the whole matrix rather than one per row: scaling rows
    /// separately would move the image, and a downmix that shifts the balance
    /// is worse than one that is quiet.
    double scale = 1.0;

    [[nodiscard]] double at(std::uint32_t out, std::uint32_t in) const noexcept
    {
        return coefficients[static_cast<std::size_t>(out) * inputs + in];
    }
    [[nodiscard]] bool identity() const noexcept;
    /// The matrix as `--dsp mix:matrix=` would take it, for a report.
    [[nodiscard]] std::string text() const;
};

/// Builds the matrix from the two layouts. `in_mask` or `out_mask` may be zero,
/// in which case the conventional layout for that channel count is used -- and
/// a channel count with no conventional layout (3, 5, 7) is refused rather than
/// guessed at.
[[nodiscard]] bool build(std::uint32_t in_channels, std::uint32_t in_mask,
                         std::uint32_t out_channels, std::uint32_t out_mask,
                         const Recipe& recipe, Matrix& out, std::string& why);

/// `1,0,0.7;0,1,0.7` -- rows are outputs, and there must be `inputs` of them
/// in each. An explicit matrix skips every rule above, which is the point of it.
[[nodiscard]] bool parse_matrix(const std::string& text, std::uint32_t inputs,
                                std::uint32_t outputs, Matrix& out, std::string& why);

[[nodiscard]] bool normalise_from_name(const std::string& name, Normalise& out);
[[nodiscard]] const char* normalise_name(Normalise n) noexcept;

/// The conventional mask for a channel count, or 0 where there is not one.
[[nodiscard]] std::uint32_t conventional_mask(std::uint32_t channels) noexcept;

} // namespace mp::mix

#endif // MEDIAPERCH_MIX_HPP

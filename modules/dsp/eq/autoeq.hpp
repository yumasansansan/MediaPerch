// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading AutoEq's correction profiles.
//
// AutoEq (https://github.com/jaakkopasanen/AutoEq, MIT) publishes a headphone
// correction for a few thousand models, in the formats Equalizer APO reads.
// Two of them matter here and they are two different kinds of thing:
//
//   ParametricEQ.txt   a preamp and a list of biquads. Maps onto `dsp_eq`'s
//                      bands one for one, so it works in every mode including
//                      the zero-latency one.
//
//     Preamp: -6.8 dB
//     Filter 1: ON PK Fc 105 Hz Gain 4.2 dB Q 0.70
//
//   GraphicEQ.txt      a correction curve, sampled at a hundred-odd
//                      frequencies. It is not a filter and it is not a
//                      cascade -- it is a *target*, and realising it needs the
//                      FIR modes, which is exactly what those are for.
//
//     GraphicEQ: 20 -1.2; 21 -1.3; 22 -1.4; ...
//
// **The preamp is part of the profile, not a suggestion.** AutoEq computes it
// so the corrected signal does not clip, and a profile applied without it is a
// profile that clips. It is applied, and reported.

#ifndef MEDIAPERCH_AUTOEQ_HPP
#define MEDIAPERCH_AUTOEQ_HPP

#include <biquad.hpp>

#include <string>
#include <utility>
#include <vector>

namespace mp::autoeq {

struct Profile {
    /// `parametric`, `graphic`, or empty when nothing was loaded.
    std::string kind;
    double preamp_db = 0.0;
    std::vector<mp::biquad::Band> bands;
    /// Hertz and decibels, in ascending frequency.
    std::vector<std::pair<double, double>> curve;

    [[nodiscard]] bool empty() const noexcept { return bands.empty() && curve.empty(); }
};

/// Sniffs which of the two it is and reads it. Anything else is refused with
/// the first line it could not make sense of.
[[nodiscard]] bool parse(const std::string& text, Profile& out, std::string& why);

/// The same, from a file. The path is UTF-8, as the ABI says -- so on Windows
/// it goes through the wide call, because half the files on a machine with a
/// Japanese code page cannot be opened by the narrow one.
[[nodiscard]] bool load(const std::string& path, Profile& out, std::string& why);

/// The curve read at any frequency, interpolated between its points on a
/// logarithmic axis -- which is the axis it was sampled on and the one the ear
/// uses. Outside its range it holds the nearest end rather than extrapolating,
/// because a correction curve says nothing about what it did not measure.
[[nodiscard]] double curve_db(const std::vector<std::pair<double, double>>& curve,
                              double hz) noexcept;

} // namespace mp::autoeq

#endif // MEDIAPERCH_AUTOEQ_HPP

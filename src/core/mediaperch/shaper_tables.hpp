// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measured noise-shaping curves, and how to find one.
//
// The coefficients themselves are generated: `tools/gen_shaper_tables.py`
// transcribes them from SSRC (https://github.com/shibatch/ssrc, Naoki Shibata,
// Boost Software License 1.0) and refuses to write a table that fails its own
// checks. They are ATH-weighted -- the absolute threshold of hearing -- and are
// indexed by sample rate as well as by intensity, because where the ear stops
// listening is a fixed frequency and where that lands in the spectrum depends
// on the rate.

#ifndef MEDIAPERCH_SHAPER_TABLES_HPP
#define MEDIAPERCH_SHAPER_TABLES_HPP

#include <cstdint>
#include <span>

namespace mp {

struct ShaperCurve {
    std::uint32_t sample_rate;
    /// SSRC's own numbering. Low is gentle, high is aggressive; 98 is a plain
    /// first-order shaper and 99 is none at all.
    std::uint32_t intensity;
    const char* name;
    const double* coefficients;
    std::uint32_t length;
};

/// Every curve the generator wrote, in the order it wrote them.
[[nodiscard]] std::span<const ShaperCurve> shaper_curves() noexcept;

/// The curve for this rate and intensity, or nullptr.
///
/// **Exact on the rate, deliberately.** A curve fitted for 44.1 kHz applied to
/// a 96 kHz stream puts its noise in the wrong place -- not subtly, but by an
/// octave -- and quietly using the nearest one would be worse than saying there
/// is none.
[[nodiscard]] const ShaperCurve* find_shaper(std::uint32_t sample_rate,
                                             std::uint32_t intensity) noexcept;

/// The intensities available at this rate, for a caller that has to offer them.
[[nodiscard]] std::uint32_t highest_intensity(std::uint32_t sample_rate) noexcept;

} // namespace mp

#endif // MEDIAPERCH_SHAPER_TABLES_HPP

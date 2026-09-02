// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loudness, as ITU-R BS.1770 and EBU R 128 define it, and the ReplayGain that
// follows from it.
//
// **ReplayGain is measured here rather than read.** A tag is a number somebody
// else's encoder wrote, in a version of the specification they chose, from a
// decode that may not match this one -- and this tree does not have a metadata
// module yet anyway. Measuring costs two second-order sections per channel and
// a running sum, which is nothing, and it produces a number this program can
// stand behind. A tag, when there is something to read one, becomes a *second*
// opinion rather than the only one.
//
// The algorithm is not negotiable and is not a preference:
//
//   K-weighting   a high shelf and a high-pass, whose parameters the standard
//                 gives as analogue frequencies -- so they are re-derived at
//                 every sample rate rather than transcribed at 48 kHz and
//                 warped elsewhere, which is the mistake that makes a meter
//                 read half a decibel out at 96 kHz.
//   400 ms blocks, overlapping by three quarters.
//   Two gates     everything below -70 LUFS absolute, then everything more than
//                 10 LU below the mean of what is left. The gates are what stop
//                 a quiet passage counting as much as the music.
//
// The one calibration point is worth stating because it is what the tests
// check: a 1 kHz sine at -23 dBFS in both channels of a stereo stream measures
// -23.0 LUFS. That is EBU Tech 3341's first conformance signal, and a meter
// that gets it wrong is wrong.

#ifndef MEDIAPERCH_LOUDNESS_HPP
#define MEDIAPERCH_LOUDNESS_HPP

#include <biquad.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mp::loudness {

/// The two sections the standard specifies, derived for this rate.
[[nodiscard]] std::vector<mp::biquad::Coefficients> k_weighting(double sample_rate);

/// What ReplayGain 2.0 aims at, in LUFS. -18 is the specification's own.
inline constexpr double k_reference_lufs = -18.0;

/// A meter. Feed it the audio and ask it what it heard.
class Meter {
public:
    /// `channel_mask` decides the weights: the standard gives the surrounds
    /// 1.41 and leaves the effects channel out of the sum entirely. A mask of
    /// zero takes the conventional layout for the channel count.
    [[nodiscard]] bool configure(double sample_rate, std::uint32_t channels,
                                 std::uint32_t channel_mask, std::string& why);

    /// Deinterleaved, and read-only: the meter never changes what it measures.
    void add(const double* const* in, std::uint32_t frames);

    /// Integrated loudness in LUFS, or `silence()` when nothing passed the
    /// absolute gate. Callable at any point; it is the answer for everything
    /// added so far.
    [[nodiscard]] double integrated_lufs() const;
    /// The loudest single sample seen, in dB relative to full scale. Sample
    /// peak, not true peak: true peak needs the signal resampled four times up,
    /// which is a resampler this stage does not have. Said rather than implied.
    [[nodiscard]] double sample_peak_db() const;
    [[nodiscard]] double sample_peak() const noexcept { return peak_; }
    /// The gain that would bring `integrated_lufs()` to `target`.
    [[nodiscard]] double replay_gain_db(double target = k_reference_lufs) const;

    [[nodiscard]] std::uint64_t blocks() const noexcept { return blocks_; }
    [[nodiscard]] static double silence() noexcept { return -70.0 - 1e9; }

    void reset();

private:
    void close_block();

    mp::biquad::Cascade filter_;
    std::vector<double> weights_;
    std::uint32_t channels_ = 0;
    double sample_rate_ = 0.0;

    /// One 400 ms block is `block_` frames and a new one starts every
    /// `step_` -- three quarters of the way through the last.
    std::uint32_t block_ = 0;
    std::uint32_t step_ = 0;
    /// The running sum of squares per channel, for each block still open.
    std::vector<std::vector<double>> partial_;
    std::vector<std::uint32_t> filled_;
    /// Frames seen, which is what decides when a block starts.
    std::uint64_t position_ = 0;

    /// Every closed block's weighted mean square. The gates need all of them,
    /// and one double per hundred milliseconds is a megabyte per day.
    std::vector<double> loudness_;
    std::uint64_t blocks_ = 0;
    double peak_ = 0.0;

    /// Scratch for the filtered block, so `add` allocates nothing after the
    /// first call.
    std::vector<double> scratch_;
    std::vector<double*> planes_;
};

} // namespace mp::loudness

#endif // MEDIAPERCH_LOUDNESS_HPP

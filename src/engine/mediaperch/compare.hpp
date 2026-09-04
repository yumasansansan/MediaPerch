// SPDX-License-Identifier: GPL-3.0-or-later
//
// Holding a decode against the audio that was encoded.
//
// Every measurement in docs/formats.md so far compares one decoder with
// another, which answers "do these agree" and cannot answer "are they both
// wrong". For a lossy codec the file that was encoded is the only reference
// outside the decoders, so it is the only thing that can catch a mistake both
// of them make.
//
// What it can and cannot show is worth stating, because it is easy to expect
// too much of it. The encoder threw information away on purpose: at 128 kbps
// the difference between the decoded audio and the source is tens of decibels,
// six or seven orders of magnitude larger than the difference between two
// correct decoders. So this **cannot rank two decoders on fidelity** -- the
// encoder's loss is common to both and swamps everything else.
//
// What it can do is the part no decoder-to-decoder comparison can:
//
//   * length, against the truth rather than against another opinion of it;
//   * alignment, which is what Media Foundation and FAAD2 both got wrong;
//   * which channel came out of which speaker;
//   * a fidelity floor that does not assume any other decoder is right.
//
// No OS headers, no allocation the caller did not ask for beyond scratch, and
// no I/O: the caller brings two blocks of interleaved float.

#ifndef MEDIAPERCH_COMPARE_HPP
#define MEDIAPERCH_COMPARE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

/// What a comparison found. Every field is a measurement; none is a verdict.
struct Comparison {
    std::uint64_t frames_reference = 0;
    std::uint64_t frames_subject = 0;
    std::uint64_t frames_compared = 0;
    unsigned channels = 0;
    std::uint32_t sample_rate = 0;

    /// A NaN or an infinity anywhere in the subject. Checked first, because
    /// every number below it is meaningless if this is false.
    bool finite = true;

    double peak_reference = 0.0;
    double peak_subject = 0.0;
    double dc_reference = 0.0; ///< mean sample value, worst channel
    double dc_subject = 0.0;

    /// 10 log10 (reference energy / error energy), over every channel.
    double snr_db = 0.0;
    double rms_error = 0.0;
    double max_abs_error = 0.0;

    /// Where the cross-correlation of the channel sum peaks, in frames, and by how
    /// much that peak stands above the lags well away from it. A decoder that
    /// starts a track late peaks somewhere other than zero; one that is right
    /// peaks at zero and clearly.
    ///
    /// This is found first and everything below is then measured on the two
    /// signals *aligned to it*. A decode that starts late is a different failure
    /// from a decode that is wrong, and measuring fidelity across a delay
    /// produces a number that is meaningless without looking meaningless. It
    /// also makes a format with no gapless metadata measurable at all: raw ADTS
    /// is correctly a whole frame late, and every other check still applies to
    /// it once that is taken out.
    int lag = 0;
    double lag_margin_db = 0.0;

    /// For each subject channel, the reference channel it correlates with best,
    /// and the worst separation between a channel's own score and its best
    /// rival's. Two channels swapped is a permutation this notices and a
    /// broadband SNR does not.
    std::vector<unsigned> best_for;
    bool channels_in_order = true;
    double channel_margin_db = 0.0;

    /// The worst band-energy disagreement below `band_limit_hz`, in decibels,
    /// and where it was. Catches a band that came out silent or eight times too
    /// loud -- which a broadband figure can hide when the band is a small part
    /// of the total energy.
    ///
    /// Bands the reference has essentially nothing in are skipped: what is in
    /// them is the analysis window's leakage from the bands that do have
    /// signal, and the ratio of two leakages is not a measurement of anything.
    /// `bands_checked` is how many survived that, and a band check that checked
    /// nothing is worth knowing about.
    double worst_band_db = 0.0;
    std::uint32_t worst_band_hz = 0;
    unsigned bands_checked = 0;
};

/// Compares `subject` with `reference`, both interleaved float of `channels`.
///
/// `band_limit_hz` bounds the spectral check; pass 0 to skip it. `max_lag`
/// bounds the alignment search in frames -- large enough to see the delay a
/// broken decoder would introduce (a whole AAC frame is 1024) and no larger,
/// because the search is linear in it. Zero says the two are known to be
/// aligned and the search should not run at all.
Comparison compare(const float* reference, std::uint64_t reference_frames, const float* subject,
                   std::uint64_t subject_frames, unsigned channels, std::uint32_t sample_rate,
                   std::uint32_t band_limit_hz, int max_lag);

/// The thresholds a comparison has to clear. Every one of them is a number
/// somebody chose, so every one of them is here rather than buried in the code.
struct Requirements {
    bool exact_length = true;    ///< the decode is as long as the file it came from
    /// And starts in the same place. Turned off for a format that carries no
    /// gapless metadata, where the encoder's delay is correctly still in the
    /// audio -- but a decode that starts *early* is wrong whatever the format,
    /// so a negative lag fails either way.
    bool lag_zero = true;
    bool channels_in_order = true;
    double min_snr_db = 0.0;     ///< a floor, not a target: it depends on the bit rate
    double min_lag_margin_db = 6.0;
    double min_channel_margin_db = 6.0;
    double max_band_db = 0.0;    ///< 0 disables the band check
    double max_peak_ratio = 2.0; ///< how far past the source's peak the decode may go
    double max_dc = 0.01;
};

/// Every requirement the comparison failed to meet, one sentence each. Empty
/// means it passed.
std::vector<std::string> failures(const Comparison& measured, const Requirements& required);

} // namespace mp

#endif // MEDIAPERCH_COMPARE_HPP

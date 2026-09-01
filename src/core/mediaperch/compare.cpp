// SPDX-License-Identifier: GPL-3.0-or-later

#include "compare.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

namespace mp {
namespace {

constexpr unsigned k_fft = 1024;

double db(double ratio) noexcept
{
    if (!(ratio > 0.0)) {
        return -999.0;
    }
    return 10.0 * std::log10(ratio);
}

/// Iterative radix-2, decimation in time. Small, and small on purpose: the band
/// check needs a spectrum and nothing else in this tree does, so this is 30
/// lines rather than a dependency. `tests/compare_test.cpp` checks it against
/// signals whose transform is known by hand.
void fft(std::complex<double>* a, unsigned n) noexcept
{
    for (unsigned i = 1, j = 0; i < n; ++i) {
        unsigned bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        const double theta = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> step{std::cos(theta), std::sin(theta)};
        for (unsigned i = 0; i < n; i += len) {
            std::complex<double> w{1.0, 0.0};
            for (unsigned k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

/// Mean energy per FFT bin, over as many windows as the signal holds.
///
/// Hann-windowed and hopped by half a window, which is the ordinary way to make
/// a stationary spectrum out of a signal: the point here is the *ratio* of two
/// such spectra, and any consistent estimator gives the same ratio.
void spectrum(const float* x, std::uint64_t frames, unsigned channels, unsigned channel,
              std::vector<double>& out)
{
    out.assign(k_fft / 2 + 1, 0.0);
    if (frames < k_fft) {
        return;
    }
    std::vector<double> window(k_fft);
    for (unsigned i = 0; i < k_fft; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / k_fft);
    }
    std::vector<std::complex<double>> buffer(k_fft);
    std::uint64_t windows = 0;
    for (std::uint64_t at = 0; at + k_fft <= frames; at += k_fft / 2) {
        for (unsigned i = 0; i < k_fft; ++i) {
            const double v = static_cast<double>(x[(at + i) * channels + channel]);
            buffer[i] = std::complex<double>{v * window[i], 0.0};
        }
        fft(buffer.data(), k_fft);
        for (unsigned k = 0; k <= k_fft / 2; ++k) {
            out[k] += std::norm(buffer[k]);
        }
        ++windows;
    }
    if (windows != 0) {
        for (double& v : out) {
            v /= static_cast<double>(windows);
        }
    }
}

/// Every channel added together.
///
/// The alignment search runs on this rather than on channel 0, and the reason
/// is a case the unit tests found before any file did: if the channels are
/// permuted, channel 0 of the decode is not channel 0 of the source, the two do
/// not correlate at any lag, and the search settles on noise. Everything
/// measured afterwards is then measured at a lag that means nothing -- including
/// the channel matrix that would have reported the permutation.
///
/// A sum is invariant under permutation, which is exactly the property needed:
/// it finds the delay whether or not the channels are in order, so the two
/// findings stay independent.
std::vector<float> downmix(const float* x, std::uint64_t frames, unsigned channels)
{
    std::vector<float> out(frames);
    for (std::uint64_t n = 0; n < frames; ++n) {
        double sum = 0.0;
        for (unsigned c = 0; c < channels; ++c) {
            sum += static_cast<double>(x[n * channels + c]);
        }
        out[n] = static_cast<float>(sum);
    }
    return out;
}

/// Correlation of two channels, normalised so that 1.0 is identity and 0.0 is
/// unrelated. Energy rather than amplitude, so it can be quoted in decibels.
double correlation(const float* a, const float* b, std::uint64_t frames, unsigned channels,
                   unsigned ca, unsigned cb, std::int64_t lag) noexcept
{
    double sum = 0.0;
    double ea = 0.0;
    double eb = 0.0;
    for (std::uint64_t n = 0; n < frames; ++n) {
        const std::int64_t m = static_cast<std::int64_t>(n) + lag;
        if (m < 0 || static_cast<std::uint64_t>(m) >= frames) {
            continue;
        }
        const double x = static_cast<double>(a[n * channels + ca]);
        const double y = static_cast<double>(b[static_cast<std::uint64_t>(m) * channels + cb]);
        sum += x * y;
        ea += x * x;
        eb += y * y;
    }
    if (ea <= 0.0 || eb <= 0.0) {
        return 0.0;
    }
    return sum / std::sqrt(ea * eb);
}

} // namespace

Comparison compare(const float* reference, std::uint64_t reference_frames, const float* subject,
                   std::uint64_t subject_frames, unsigned channels, std::uint32_t sample_rate,
                   std::uint32_t band_limit_hz, int max_lag)
{
    Comparison out;
    out.frames_reference = reference_frames;
    out.frames_subject = subject_frames;
    out.channels = channels;
    out.sample_rate = sample_rate;
    const std::uint64_t overlap = std::min(reference_frames, subject_frames);
    if (channels == 0 || overlap == 0 || reference == nullptr || subject == nullptr) {
        out.finite = false;
        return out;
    }
    for (std::uint64_t i = 0; i < subject_frames * channels; ++i) {
        if (!std::isfinite(subject[i])) {
            out.finite = false;
            return out;
        }
    }

    // Alignment comes first, because every number after it is measured on the
    // aligned signals. A decode that starts late is a *separate* failure from a
    // decode that is wrong, and mixing the two produces the worst kind of
    // result: a fidelity figure that is meaningless and does not look it.
    const std::vector<float> flat_reference = downmix(reference, reference_frames, channels);
    const std::vector<float> flat_subject = downmix(subject, subject_frames, channels);
    double best = -2.0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        const double c =
            correlation(flat_reference.data(), flat_subject.data(), overlap, 1, 0, 0, lag);
        if (c > best) {
            best = c;
            out.lag = lag;
        }
    }
    // The margin is measured against lags well away from the peak, not the ones
    // next to it. Any band-limited signal correlates almost as well one sample
    // out as it does at zero -- that is what band-limited means -- so comparing
    // the peak with its own neighbour measures the *signal's* bandwidth and not
    // whether the alignment was found. Sixteen frames is past the shoulder of
    // anything these files contain and far short of a codec delay.
    constexpr int k_guard = 16;
    double elsewhere = -2.0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        if (std::abs(lag - out.lag) < k_guard) {
            continue;
        }
        elsewhere = std::max(elsewhere, correlation(flat_reference.data(), flat_subject.data(),
                                                    overlap, 1, 0, 0, lag));
    }
    out.lag_margin_db = db(std::max(best, 0.0) / std::max(elsewhere, 1e-12));

    // From here on, `reference` and `subject` are the aligned views: the same
    // audio, wherever the decode put it. `n` is how much of it there is.
    const std::int64_t lag = out.lag;
    const std::uint64_t skip_reference = lag < 0 ? static_cast<std::uint64_t>(-lag) : 0;
    const std::uint64_t skip_subject = lag > 0 ? static_cast<std::uint64_t>(lag) : 0;
    if (skip_reference >= reference_frames || skip_subject >= subject_frames) {
        out.finite = false;
        return out;
    }
    reference += skip_reference * channels;
    subject += skip_subject * channels;
    const std::uint64_t n =
        std::min(reference_frames - skip_reference, subject_frames - skip_subject);
    out.frames_compared = n;

    double error_energy = 0.0;
    double reference_energy = 0.0;
    for (std::uint64_t i = 0; i < n * channels; ++i) {
        const double r = static_cast<double>(reference[i]);
        const double s = static_cast<double>(subject[i]);
        const double d = s - r;
        error_energy += d * d;
        reference_energy += r * r;
        out.peak_reference = std::max(out.peak_reference, std::abs(r));
        out.peak_subject = std::max(out.peak_subject, std::abs(s));
        out.max_abs_error = std::max(out.max_abs_error, std::abs(d));
    }
    out.rms_error = std::sqrt(error_energy / static_cast<double>(n * channels));
    out.snr_db = error_energy > 0.0 ? db(reference_energy / error_energy) : 999.0;

    for (unsigned c = 0; c < channels; ++c) {
        double sr = 0.0;
        double ss = 0.0;
        for (std::uint64_t f = 0; f < n; ++f) {
            sr += static_cast<double>(reference[f * channels + c]);
            ss += static_cast<double>(subject[f * channels + c]);
        }
        out.dc_reference = std::max(out.dc_reference, std::abs(sr) / static_cast<double>(n));
        out.dc_subject = std::max(out.dc_subject, std::abs(ss) / static_cast<double>(n));
    }

    // Which channel came out where. Correlating every subject channel against
    // every reference channel is the only way to tell a permutation from a
    // decode that is simply wrong, and it is what caught Media Foundation
    // rearranging multichannel ALAC.
    out.best_for.assign(channels, 0);
    out.channel_margin_db = 999.0;
    for (unsigned c = 0; c < channels; ++c) {
        double own = 0.0;
        double other = 0.0;
        unsigned winner = 0;
        double winning = -2.0;
        for (unsigned d = 0; d < channels; ++d) {
            const double score = std::abs(correlation(reference, subject, n, channels, d, c, 0));
            if (score > winning) {
                winning = score;
                winner = d;
            }
            if (d == c) {
                own = score;
            } else {
                other = std::max(other, score);
            }
        }
        out.best_for[c] = winner;
        if (winner != c) {
            out.channels_in_order = false;
        }
        out.channel_margin_db =
            std::min(out.channel_margin_db, db(std::max(own, 0.0) / std::max(other, 1e-12)));
    }

    // Band energies. Averaged across channels, because a per-channel figure
    // would be noisier without saying anything the total does not.
    if (band_limit_hz != 0 && sample_rate != 0 && n >= k_fft) {
        const unsigned top = std::min<unsigned>(
            k_fft / 2, static_cast<unsigned>(static_cast<std::uint64_t>(band_limit_hz) * k_fft /
                                             sample_rate));
        std::vector<double> sr;
        std::vector<double> ss;
        std::vector<double> total_r(k_fft / 2 + 1, 0.0);
        std::vector<double> total_s(k_fft / 2 + 1, 0.0);
        for (unsigned c = 0; c < channels; ++c) {
            spectrum(reference, n, channels, c, sr);
            spectrum(subject, n, channels, c, ss);
            for (unsigned k = 0; k <= k_fft / 2; ++k) {
                total_r[k] += sr[k];
                total_s[k] += ss[k];
            }
        }
        // A band the source has almost nothing in cannot say anything about a
        // decoder: what is there is the window's own leakage from the bands
        // that do have signal, and the ratio of two leakages is noise. Sixty
        // decibels below the loudest band is the line -- the unit tests found
        // this by reporting a 9 dB disagreement in a band that held a
        // millionth of the energy, next to the 6 dB one that was real.
        double loudest = 0.0;
        for (unsigned k = 0; k < top; ++k) {
            loudest = std::max(loudest, total_r[k]);
        }
        const double floor_energy = loudest * 1e-6;

        // Sixteen bins to a band: fine enough to see a hole, coarse enough that
        // one noisy bin does not become a failure.
        constexpr unsigned k_group = 16;
        for (unsigned lo = 0; lo + k_group <= top; lo += k_group) {
            double er = 0.0;
            double es = 0.0;
            for (unsigned k = lo; k < lo + k_group; ++k) {
                er += total_r[k];
                es += total_s[k];
            }
            if (er <= 0.0 || er < floor_energy * k_group) {
                continue;
            }
            ++out.bands_checked;
            const double diff = std::abs(db(es / er));
            if (diff > out.worst_band_db) {
                out.worst_band_db = diff;
                out.worst_band_hz = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(lo + k_group / 2) * sample_rate / k_fft);
            }
        }
    }

    return out;
}

std::vector<std::string> failures(const Comparison& measured, const Requirements& required)
{
    std::vector<std::string> out;
    char line[256];

    if (!measured.finite) {
        out.emplace_back("the decode contains a NaN or an infinity");
        return out; // nothing else measured means anything
    }
    if (required.exact_length && measured.frames_subject != measured.frames_reference) {
        std::snprintf(line, sizeof(line), "length %llu, but the source is %llu",
                      static_cast<unsigned long long>(measured.frames_subject),
                      static_cast<unsigned long long>(measured.frames_reference));
        out.emplace_back(line);
    }
    if (required.lag_zero && measured.lag != 0) {
        std::snprintf(line, sizeof(line), "starts %d frames from where the source does",
                      measured.lag);
        out.emplace_back(line);
    } else if (measured.lag < 0) {
        std::snprintf(line, sizeof(line), "starts %d frames *before* the source does",
                      measured.lag);
        out.emplace_back(line);
    }
    if (measured.lag_margin_db < required.min_lag_margin_db) {
        std::snprintf(line, sizeof(line),
                      "alignment is ambiguous: the peak stands only %.2f dB above the next lag, "
                      "and %.2f was required",
                      measured.lag_margin_db, required.min_lag_margin_db);
        out.emplace_back(line);
    }
    if (required.channels_in_order && !measured.channels_in_order) {
        std::string where = "channels are not in the source's order:";
        for (unsigned c = 0; c < measured.best_for.size(); ++c) {
            std::snprintf(line, sizeof(line), " %u<-%u", c, measured.best_for[c]);
            where += line;
        }
        out.emplace_back(where);
    }
    if (measured.channel_margin_db < required.min_channel_margin_db) {
        std::snprintf(line, sizeof(line),
                      "a channel is only %.2f dB better matched by its own source channel than by "
                      "another, and %.2f was required",
                      measured.channel_margin_db, required.min_channel_margin_db);
        out.emplace_back(line);
    }
    if (measured.snr_db < required.min_snr_db) {
        std::snprintf(line, sizeof(line), "%.2f dB against the source, and %.2f was required",
                      measured.snr_db, required.min_snr_db);
        out.emplace_back(line);
    }
    if (required.max_band_db > 0.0 && measured.bands_checked != 0 &&
        measured.worst_band_db > required.max_band_db) {
        std::snprintf(line, sizeof(line),
                      "the band at %u Hz is %.2f dB from the source's, and %.2f was allowed",
                      measured.worst_band_hz, measured.worst_band_db, required.max_band_db);
        out.emplace_back(line);
    }
    if (measured.peak_reference > 0.0 &&
        measured.peak_subject > measured.peak_reference * required.max_peak_ratio) {
        std::snprintf(line, sizeof(line), "peaks at %.4f where the source peaks at %.4f",
                      measured.peak_subject, measured.peak_reference);
        out.emplace_back(line);
    }
    if (std::abs(measured.dc_subject - measured.dc_reference) > required.max_dc) {
        std::snprintf(line, sizeof(line), "direct current %.5f against the source's %.5f",
                      measured.dc_subject, measured.dc_reference);
        out.emplace_back(line);
    }
    return out;
}

} // namespace mp

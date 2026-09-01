// SPDX-License-Identifier: GPL-3.0-or-later

#include "loudness.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cmath>

namespace mp::loudness {
namespace {

constexpr double k_pi = 3.14159265358979323846;
/// The standard's own offset, which is what puts a 1 kHz sine at -23 dBFS at
/// -23.0 LUFS rather than somewhere near it.
constexpr double k_offset = -0.691;
constexpr double k_absolute_gate = -70.0;
constexpr double k_relative_gate = -10.0;

} // namespace

std::vector<mp::biquad::Coefficients> k_weighting(double sample_rate)
{
    std::vector<mp::biquad::Coefficients> out(2);

    // Stage 1: the head shelf. The specification gives it as an analogue
    // filter, and these are its parameters -- so the bilinear transform is
    // applied at whatever rate the audio arrives at.
    {
        constexpr double f0 = 1681.974450955533;
        constexpr double gain_db = 3.999843853973347;
        constexpr double q = 0.7071752369554196;
        const double k = std::tan(k_pi * f0 / sample_rate);
        const double vh = std::pow(10.0, gain_db / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        out[0].b0 = (vh + vb * k / q + k * k) / a0;
        out[0].b1 = 2.0 * (k * k - vh) / a0;
        out[0].b2 = (vh - vb * k / q + k * k) / a0;
        out[0].a1 = 2.0 * (k * k - 1.0) / a0;
        out[0].a2 = (1.0 - k / q + k * k) / a0;
    }

    // Stage 2: the RLB high-pass.
    {
        constexpr double f0 = 38.13547087602444;
        constexpr double q = 0.5003270373238773;
        const double k = std::tan(k_pi * f0 / sample_rate);
        const double denominator = 1.0 + k / q + k * k;
        out[1].b0 = 1.0;
        out[1].b1 = -2.0;
        out[1].b2 = 1.0;
        out[1].a1 = 2.0 * (k * k - 1.0) / denominator;
        out[1].a2 = (1.0 - k / q + k * k) / denominator;
    }
    return out;
}

bool Meter::configure(double sample_rate, std::uint32_t channels,
                      std::uint32_t channel_mask, std::string& why)
{
    if (sample_rate <= 0.0 || channels == 0 || channels > 64) {
        why = "a meter needs a rate and some channels";
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;
    filter_.set_sections(k_weighting(sample_rate), channels);

    // The weights the standard gives: the surrounds count for more and the
    // effects channel does not count at all.
    weights_.assign(channels, 1.0);
    std::uint32_t mask = channel_mask;
    if (mask == 0) {
        switch (channels) {
        case 1:
            mask = MP_SPEAKER_FRONT_CENTER;
            break;
        case 2:
            mask = MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT;
            break;
        case 6:
            mask = MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT |
                   MP_SPEAKER_FRONT_CENTER | MP_SPEAKER_LOW_FREQUENCY |
                   MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;
            break;
        default:
            mask = 0;
            break;
        }
    }
    if (mask != 0) {
        std::uint32_t at = 0;
        for (std::uint32_t bit = 1; bit != 0 && at < channels; bit <<= 1) {
            if ((mask & bit) == 0) {
                continue;
            }
            if (bit == MP_SPEAKER_LOW_FREQUENCY) {
                weights_[at] = 0.0; // out of the sum entirely
            } else if (bit == MP_SPEAKER_BACK_LEFT || bit == MP_SPEAKER_BACK_RIGHT ||
                       bit == MP_SPEAKER_SIDE_LEFT || bit == MP_SPEAKER_SIDE_RIGHT) {
                weights_[at] = 1.41;
            }
            ++at;
        }
    }

    // 400 ms, a new one every 100 ms.
    block_ = static_cast<std::uint32_t>(std::llround(sample_rate * 0.4));
    step_ = static_cast<std::uint32_t>(std::llround(sample_rate * 0.1));
    if (block_ == 0 || step_ == 0) {
        why = "that rate is too low to have a four-hundred-millisecond block";
        return false;
    }
    reset();
    return true;
}

void Meter::reset()
{
    filter_.reset();
    partial_.clear();
    filled_.clear();
    loudness_.clear();
    position_ = 0;
    blocks_ = 0;
    peak_ = 0.0;
}

void Meter::close_block()
{
    // The weighted mean square of the oldest open block, which is what a
    // gate later compares.
    double sum = 0.0;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        sum += weights_[c] * partial_.front()[c] / static_cast<double>(block_);
    }
    loudness_.push_back(sum);
    ++blocks_;
    partial_.erase(partial_.begin());
    filled_.erase(filled_.begin());
}

void Meter::add(const double* const* in, std::uint32_t frames)
{
    if (frames == 0 || in == nullptr || channels_ == 0) {
        return;
    }
    if (scratch_.size() < static_cast<std::size_t>(channels_) * frames) {
        scratch_.assign(static_cast<std::size_t>(channels_) * frames, 0.0);
        planes_.resize(channels_);
    }
    for (std::uint32_t c = 0; c < channels_; ++c) {
        planes_[c] = scratch_.data() + static_cast<std::size_t>(c) * frames;
        // The peak is of what came in, not of what the weighting made of it.
        const double* src = in[c];
        for (std::uint32_t n = 0; n < frames; ++n) {
            const double magnitude = src[n] < 0.0 ? -src[n] : src[n];
            peak_ = std::max(peak_, magnitude);
        }
    }
    filter_.process(in, frames, planes_.data());

    for (std::uint32_t n = 0; n < frames; ++n) {
        // A new block every `step_` frames, so four of them overlap at any
        // moment and each one covers 400 ms.
        if (position_ % step_ == 0) {
            partial_.emplace_back(channels_, 0.0);
            filled_.push_back(0);
        }
        for (std::size_t b = 0; b < partial_.size(); ++b) {
            for (std::uint32_t c = 0; c < channels_; ++c) {
                const double v = planes_[c][n];
                partial_[b][c] += v * v;
            }
            ++filled_[b];
        }
        while (!filled_.empty() && filled_.front() >= block_) {
            close_block();
        }
        ++position_;
    }
}

double Meter::integrated_lufs() const
{
    if (loudness_.empty()) {
        return silence();
    }
    // The absolute gate first.
    double sum = 0.0;
    std::size_t count = 0;
    for (const double z : loudness_) {
        if (z > 0.0 && k_offset + 10.0 * std::log10(z) > k_absolute_gate) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) {
        return silence();
    }
    // Then the relative one, from the mean of what survived.
    const double relative =
        k_offset + 10.0 * std::log10(sum / static_cast<double>(count)) + k_relative_gate;
    double gated = 0.0;
    std::size_t kept = 0;
    for (const double z : loudness_) {
        if (z > 0.0 && k_offset + 10.0 * std::log10(z) > k_absolute_gate &&
            k_offset + 10.0 * std::log10(z) > relative) {
            gated += z;
            ++kept;
        }
    }
    if (kept == 0) {
        return silence();
    }
    return k_offset + 10.0 * std::log10(gated / static_cast<double>(kept));
}

double Meter::sample_peak_db() const
{
    return peak_ > 0.0 ? 20.0 * std::log10(peak_) : -400.0;
}

double Meter::replay_gain_db(double target) const
{
    const double measured = integrated_lufs();
    return measured <= silence() / 2.0 ? 0.0 : target - measured;
}

} // namespace mp::loudness

// SPDX-License-Identifier: GPL-3.0-or-later

#include "loudness.hpp"

#include <mediaperch/module.h>
#include <peak.hpp>
#include <transform.hpp>

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

/// The gates as powers, which is what a block's weighted mean square is: a
/// block is louder than L LUFS when its power is above 10^((L - offset) / 10),
/// and 10 LU below the mean of the blocks is a tenth of their mean power. So
/// the gates compare powers, as the standard's equations do before they take
/// a logarithm, and no block's logarithm is needed at all.
const double k_absolute_power = std::pow(10.0, (k_absolute_gate - k_offset) / 10.0);
const double k_relative_power = std::pow(10.0, k_relative_gate / 10.0);

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
    // As many blocks as can be open at once: one opens every step and each
    // lasts a block, and the oldest closes before the next one opens.
    slots_ = (block_ + step_ - 1) / step_;
    partial_.assign(slots_ * channels_, 0.0);
    filled_.assign(slots_, 0);
    reset();
    return true;
}

void Meter::reset()
{
    filter_.reset();
    head_ = 0;
    open_ = 0;
    loudness_.clear();
    absolute_sum_ = 0.0;
    absolute_count_ = 0;
    position_ = 0;
    blocks_ = 0;
    peak_ = 0.0;
}

void Meter::close_block()
{
    // The weighted mean square of the oldest open block, which is what a
    // gate later compares.
    // The block's length is divided out once, where each channel's share was
    // divided by it: one rounding where there were as many as channels.
    const double* oldest = partial_.data() + head_ * channels_;
    double weighted = 0.0;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        weighted += weights_[c] * oldest[c];
    }
    const double power = weighted / static_cast<double>(block_);
    loudness_.push_back(power);
    if (power > k_absolute_power) {
        absolute_sum_ += power;
        ++absolute_count_;
    }
    ++blocks_;
    head_ = head_ + 1 == slots_ ? 0 : head_ + 1;
    --open_;
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
        peak_ = mp::peak::loudest(in[c], frames, peak_);
    }
    filter_.process(in, frames, planes_.data());

    // **A run of frames at a time, not a frame.** Until the next block starts
    // or the oldest one is full, every open block takes the same frames, so a
    // channel's sum of squares over the run is made once, by transform::dot,
    // and added to each of them. Frame by frame it was one running sum per
    // block and channel, which the compiler may not split.
    // The `b`th open block's place in the ring, oldest first.
    const auto slot = [this](std::size_t b) {
        const std::size_t at = head_ + b;
        return at < slots_ ? at : at - slots_;
    };
    std::uint32_t n = 0;
    while (n < frames) {
        // A new block every `step_` frames, so four of them overlap at any
        // moment and each one covers 400 ms.
        if (position_ % step_ == 0) {
            const std::size_t at = slot(open_);
            std::fill_n(partial_.data() + at * channels_, channels_, 0.0);
            filled_[at] = 0;
            ++open_;
        }
        std::uint64_t run = std::min<std::uint64_t>(step_ - position_ % step_, frames - n);
        if (open_ != 0) {
            run = std::min<std::uint64_t>(run, block_ - filled_[head_]);
        }
        const auto length = static_cast<std::uint32_t>(run);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double* v = planes_[c] + n;
            const double squares = mp::transform::dot(v, v, length);
            for (std::size_t b = 0; b < open_; ++b) {
                partial_[slot(b) * channels_ + c] += squares;
            }
        }
        for (std::size_t b = 0; b < open_; ++b) {
            filled_[slot(b)] += length;
        }
        while (open_ != 0 && filled_[head_] >= block_) {
            close_block();
        }
        position_ += length;
        n += length;
    }
}

double Meter::integrated_lufs() const
{
    // The absolute gate was applied as each block closed.
    if (absolute_count_ == 0) {
        return silence();
    }
    // Then the relative one, from the mean power of what survived. Both
    // compare powers, so a pass over every block is comparisons and sums: it
    // took one to three logarithms of each block, every time it was asked.
    const double relative =
        absolute_sum_ / static_cast<double>(absolute_count_) * k_relative_power;
    double gated = 0.0;
    std::size_t kept = 0;
    for (const double power : loudness_) {
        if (power > k_absolute_power && power > relative) {
            gated += power;
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

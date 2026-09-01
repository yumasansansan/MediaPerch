// SPDX-License-Identifier: GPL-3.0-or-later

#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mp::resample {
namespace {

constexpr double k_pi = 3.14159265358979323846;

/// The zeroth-order modified Bessel function, by its series.
///
/// It converges quickly for the arguments a Kaiser window uses (beta is under
/// 20 for any attenuation anybody wants), and the series is four lines. A table
/// would be four lines and a transcription risk.
double bessel_i0(double x) noexcept
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18) {
            break;
        }
    }
    return sum;
}

double sinc(double x) noexcept
{
    if (x == 0.0) {
        return 1.0;
    }
    const double t = k_pi * x;
    return std::sin(t) / t;
}

/// Kaiser's own formula for the shape parameter, from the attenuation.
double kaiser_beta(double attenuation_db) noexcept
{
    if (attenuation_db > 50.0) {
        return 0.1102 * (attenuation_db - 8.7);
    }
    if (attenuation_db >= 21.0) {
        return 0.5842 * std::pow(attenuation_db - 21.0, 0.4) +
               0.07886 * (attenuation_db - 21.0);
    }
    return 0.0;
}

std::uint32_t gcd(std::uint32_t a, std::uint32_t b) noexcept
{
    while (b != 0) {
        const std::uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

} // namespace

bool design_from_name(const std::string& name, Design& out)
{
    // The transition band is what costs taps, so it is the number that moves
    // between these, and the attenuation is chosen against a destination width.
    if (name == "fast") {
        out.attenuation_db = 96.0;
        out.bandwidth = 0.91;
    } else if (name == "good") {
        out.attenuation_db = 120.0;
        out.bandwidth = 0.95;
    } else if (name == "best") {
        out.attenuation_db = 144.0;
        out.bandwidth = 0.98;
    } else {
        return false;
    }
    return true;
}

std::string quality_names()
{
    return "fast (96 dB, 91%), good (120 dB, 95%), best (144 dB, 98%)";
}

bool Resampler::configure(std::uint32_t in_rate, std::uint32_t out_rate,
                          std::uint32_t channels, const Design& design, std::string& why)
{
    reset();
    if (in_rate == 0 || out_rate == 0 || channels == 0) {
        why = "a rate of zero is not a rate";
        return false;
    }
    if (design.bandwidth <= 0.0 || design.bandwidth >= 1.0) {
        why = "bandwidth must be between 0 and 1, exclusive";
        return false;
    }

    channels_ = channels;
    const std::uint32_t g = gcd(in_rate, out_rate);
    up_ = out_rate / g;
    down_ = in_rate / g;

    // The 1:1 case is designed like any other rather than special-cased away.
    // What comes out is a unit impulse -- an integer centre and a cutoff at
    // Nyquist put every other tap on a zero of the sinc -- and a test asserts
    // exactly that, because "resampling to the same rate changes nothing" is a
    // claim about the filter and not about a branch. `process` still takes the
    // branch: doing the arithmetic to rediscover it per sample would be silly.
    //
    // In cycles per sample of the intermediate rate, which runs at in_rate * up.
    // The lower of the two Nyquist frequencies is the one that has to be
    // protected: upsampling must not let images through, downsampling must not
    // let anything above the new Nyquist fold back.
    const double cutoff = 0.5 / static_cast<double>(std::max(up_, down_));
    const double transition = 2.0 * (1.0 - design.bandwidth) * cutoff;
    const double beta = kaiser_beta(design.attenuation_db);

    // Kaiser's order estimate. The +1 and the rounding up are the difference
    // between meeting the specification and nearly meeting it.
    const double order =
        (design.attenuation_db - 8.0) / (2.285 * 2.0 * k_pi * transition) + 1.0;
    std::uint64_t taps =
        static_cast<std::uint64_t>(std::ceil(order / static_cast<double>(up_)));
    taps = std::max<std::uint64_t>(taps, 8);
    taps += taps & 1u; // even, so the centre lands on a sample

    if (taps * up_ > design.max_taps) {
        why = "the ratio " + std::to_string(out_rate) + "/" + std::to_string(in_rate) +
              " reduces to " + std::to_string(up_) + "/" + std::to_string(down_) +
              ", which needs " + std::to_string(taps * up_) +
              " coefficients. This is a rational resampler and that is more than it "
              "will build; pick rates with a common factor, or an arbitrary-ratio "
              "resampler, which this is not";
        return false;
    }
    taps_ = static_cast<std::uint32_t>(taps);

    const std::size_t length = static_cast<std::size_t>(taps_) * up_;
    centre_ = length / 2;
    const double c = static_cast<double>(centre_);

    // A symmetric design of length+1 with its last tap -- which is exactly zero
    // -- dropped. That keeps the window and the sinc centred on the same sample,
    // which is what makes the response linear-phase and the 1:1 case exact.
    proto_.resize(length);
    const double i0_beta = bessel_i0(beta);
    for (std::size_t n = 0; n < length; ++n) {
        const double offset = static_cast<double>(n) - c;
        const double ratio = offset / c;
        const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) /
                              i0_beta;
        proto_[n] = 2.0 * cutoff * sinc(2.0 * cutoff * offset) * window;
    }

    // Unity gain at DC. The whole prototype is scaled rather than each phase
    // separately: normalising the phases one at a time would flatten DC by
    // bending the response that was just designed.
    const double sum = std::accumulate(proto_.begin(), proto_.end(), 0.0);
    if (sum != 0.0) {
        const double scale = static_cast<double>(up_) / sum;
        for (double& tap : proto_) {
            tap *= scale;
        }
    }

    // Per phase, and reversed, so the inner loop walks both arrays forwards.
    coef_.assign(length, 0.0);
    for (std::uint32_t p = 0; p < up_; ++p) {
        for (std::uint32_t j = 0; j < taps_; ++j) {
            coef_[static_cast<std::size_t>(p) * taps_ + j] =
                proto_[p + static_cast<std::size_t>(taps_ - 1 - j) * up_];
        }
    }

    // The stream starts with the filter half full of silence, which is what
    // makes output frame 0 line up with input frame 0.
    hist_.assign(channels_, std::vector<double>(taps_ - 1, 0.0));
    held_ = taps_ - 1;
    base_ = -static_cast<std::int64_t>(taps_ - 1);
    return true;
}

void Resampler::reset() noexcept
{
    for (auto& channel : hist_) {
        channel.assign(taps_ > 1 ? taps_ - 1 : 0, 0.0);
    }
    held_ = taps_ > 1 ? taps_ - 1 : 0;
    base_ = -static_cast<std::int64_t>(held_);
    out_k_ = 0;
    taken_ = 0;
    flushed_ = false;
}

std::uint32_t Resampler::max_output(std::uint32_t in_frames) const noexcept
{
    if (identity()) {
        return in_frames;
    }
    const std::uint64_t n =
        (static_cast<std::uint64_t>(in_frames) * up_ + down_ - 1) / down_ + 1;
    return static_cast<std::uint32_t>(n);
}

void Resampler::produce(std::uint64_t limit, double* const* out, std::uint32_t capacity,
                        std::uint32_t& produced)
{
    const std::uint64_t available = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(base_) + static_cast<std::int64_t>(held_));

    while (produced < capacity && out_k_ < limit) {
        const std::uint64_t m = out_k_ * down_ + centre_;
        const std::uint64_t i = m / up_;
        if (i >= available) {
            break; // the newest tap has not arrived
        }
        const auto p = static_cast<std::uint32_t>(m - i * up_);
        const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(i) - base_);
        if (at + 1 < taps_) {
            break; // the oldest tap was discarded, which would be a bug here
        }
        const double* co = coef_.data() + static_cast<std::size_t>(p) * taps_;

        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double* x = hist_[c].data() + at + 1 - taps_;
            double acc = 0.0;
            for (std::uint32_t j = 0; j < taps_; ++j) {
                acc += co[j] * x[j];
            }
            out[c][produced] = acc;
        }
        ++out_k_;
        ++produced;
    }
}

void Resampler::discard()
{
    const std::uint64_t m = out_k_ * down_ + centre_;
    const std::int64_t oldest =
        static_cast<std::int64_t>(m / up_) - static_cast<std::int64_t>(taps_) + 1;
    const std::int64_t drop = oldest - base_;
    if (drop <= 0) {
        return;
    }
    const auto n = static_cast<std::size_t>(drop);
    for (auto& channel : hist_) {
        channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(n));
    }
    held_ -= n;
    base_ += drop;
}

bool Resampler::process(const double* const* in, std::uint32_t in_frames,
                        double* const* out, std::uint32_t capacity,
                        std::uint32_t& produced)
{
    produced = 0;
    if (identity()) {
        if (in_frames > capacity) {
            return false;
        }
        for (std::uint32_t c = 0; c < channels_; ++c) {
            std::copy_n(in[c], in_frames, out[c]);
        }
        produced = in_frames;
        taken_ += in_frames;
        return true;
    }

    if (in_frames != 0 && in != nullptr) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            hist_[c].insert(hist_[c].end(), in[c], in[c] + in_frames);
        }
        held_ += in_frames;
        taken_ += in_frames;
    }

    produce(UINT64_MAX, out, capacity, produced);
    discard();
    return true;
}

bool Resampler::flush(double* const* out, std::uint32_t capacity, std::uint32_t& produced)
{
    produced = 0;
    if (identity()) {
        return true;
    }

    // Exactly as many output frames as the ratio says the input was worth. A
    // filter's tail is real audio and dropping it truncates every file; a
    // filter's tail is also finite, and running past it would pad every file.
    const std::uint64_t limit = (taken_ * up_ + down_ - 1) / down_;

    if (!flushed_) {
        // The tail exists because the last real samples are still walking out
        // through the taps. Feeding silence is what walks them out.
        flushed_ = true;
        for (auto& channel : hist_) {
            channel.insert(channel.end(), taps_, 0.0);
        }
        held_ += taps_;
    }

    produce(limit, out, capacity, produced);
    discard();
    return true;
}

} // namespace mp::resample

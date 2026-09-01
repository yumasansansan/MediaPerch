// SPDX-License-Identifier: GPL-3.0-or-later

#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mp::resample {
namespace {

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
    } else if (name == "extreme") {
        // Past every container this program can write to, and past what the
        // arithmetic under it is exact to. Here because "how far does this go"
        // is a fair question and guessing at the answer is not.
        out.attenuation_db = 180.0;
        out.bandwidth = 0.99;
    } else {
        return false;
    }
    return true;
}

std::string quality_names()
{
    return "fast (96 dB, 91%), good (120 dB, 95%), best (144 dB, 98%), "
           "extreme (180 dB, 99%)";
}

bool Resampler::configure(std::uint32_t in_rate, std::uint32_t out_rate,
                          std::uint32_t channels, const Design& design, std::string& why)
{
    reset();
    if (in_rate == 0 || out_rate == 0 || channels == 0) {
        why = "a rate of zero is not a rate";
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
    const bool same = designed_ && last_ == design && last_in_rate_ == in_rate &&
                      last_out_rate_ == out_rate;
    if (!same) {
        if (!design_prototype(design, up_, down_, proto_, taps_, response_, why)) {
            designed_ = false;
            return false;
        }
        last_ = design;
        last_in_rate_ = in_rate;
        last_out_rate_ = out_rate;
        designed_ = true;
    }
    phase_taps_ = taps_ + 1;
    centre_ = (proto_.size() - 1) / 2;

    if (same) {
        // Same filter, same phases: only the stream state has to start again.
        held_ = phase_taps_ - 1;
        hist_.assign(channels_, std::vector<double>(held_, 0.0));
        base_ = -static_cast<std::int64_t>(held_);
        return true;
    }

    // Per phase, and reversed, so the inner loop walks both arrays forwards.
    // Phase 0 is the one that reaches the prototype's last coefficient; the
    // others are padded with the zero that keeps every phase the same length.
    coef_.assign(static_cast<std::size_t>(up_) * phase_taps_, 0.0);
    for (std::uint32_t p = 0; p < up_; ++p) {
        for (std::uint32_t j = 0; j < phase_taps_; ++j) {
            const std::size_t index =
                p + static_cast<std::size_t>(phase_taps_ - 1 - j) * up_;
            if (index < proto_.size()) {
                coef_[static_cast<std::size_t>(p) * phase_taps_ + j] = proto_[index];
            }
        }
    }

    // The stream starts with the filter half full of silence, which is what
    // makes output frame 0 line up with input frame 0.
    held_ = phase_taps_ - 1;
    hist_.assign(channels_, std::vector<double>(held_, 0.0));
    base_ = -static_cast<std::int64_t>(held_);
    return true;
}

void Resampler::reset() noexcept
{
    const std::size_t seed = phase_taps_ > 1 ? phase_taps_ - 1 : 0;
    for (auto& channel : hist_) {
        channel.assign(seed, 0.0);
    }
    held_ = seed;
    base_ = -static_cast<std::int64_t>(seed);
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
    const auto available =
        static_cast<std::uint64_t>(base_ + static_cast<std::int64_t>(held_));

    while (produced < capacity && out_k_ < limit) {
        const std::uint64_t m = out_k_ * down_ + centre_;
        const std::uint64_t i = m / up_;
        if (i >= available) {
            break; // the newest tap has not arrived
        }
        const auto p = static_cast<std::uint32_t>(m - i * up_);
        const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(i) - base_);
        if (at + 1 < phase_taps_) {
            break; // the oldest tap was discarded, which would be a bug here
        }
        const double* co = coef_.data() + static_cast<std::size_t>(p) * phase_taps_;

        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double* x = hist_[c].data() + at + 1 - phase_taps_;
            double acc = 0.0;
            for (std::uint32_t j = 0; j < phase_taps_; ++j) {
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
        static_cast<std::int64_t>(m / up_) - static_cast<std::int64_t>(phase_taps_) + 1;
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
        if (in != nullptr) {
            for (std::uint32_t c = 0; c < channels_; ++c) {
                std::copy_n(in[c], in_frames, out[c]);
            }
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
            channel.insert(channel.end(), phase_taps_, 0.0);
        }
        held_ += phase_taps_;
    }

    produce(limit, out, capacity, produced);
    discard();
    return true;
}

} // namespace mp::resample

// SPDX-License-Identifier: GPL-3.0-or-later

#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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
    minimum_phase_ = design.phase == Phase::minimum;
    // A linear-phase filter's delay is exactly half of it, and sampling the
    // output from there removes it. A minimum-phase filter's energy is already
    // at the front, so there is nothing to skip past -- and nothing to remove
    // either, because its delay is a different number at every frequency. What
    // is reported instead is where the energy is.
    centre_ = minimum_phase_ ? 0 : (proto_.size() - 1) / 2;
    {
        double weight = 0.0;
        double moment = 0.0;
        for (std::size_t i = 0; i < proto_.size(); ++i) {
            const double energy = proto_[i] * proto_[i];
            weight += energy;
            moment += energy * static_cast<double>(i);
        }
        const double centroid = weight > 0.0 ? moment / weight : 0.0;
        latency_ = minimum_phase_ ? centroid / static_cast<double>(up_) : 0.0;
    }

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


// --------------------------------------------------------------------------
// Planning a cascade
// --------------------------------------------------------------------------

namespace {

constexpr double k_pi = 3.14159265358979323846;

std::vector<std::uint32_t> divisors(std::uint32_t n)
{
    std::vector<std::uint32_t> out;
    for (std::uint32_t d = 1; d * d <= n; ++d) {
        if (n % d == 0) {
            out.push_back(d);
            if (d != n / d) {
                out.push_back(n / d);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Taps per phase this stage would need, by the same order estimate the design
/// uses. Planning has to predict what designing will do, and this is that
/// prediction; it is never used as the filter.
std::uint64_t estimate_taps(double attenuation_db, double bandwidth, std::uint32_t up,
                            std::uint32_t down)
{
    const double transition = (1.0 - bandwidth) / static_cast<double>(std::max(up, down));
    if (transition <= 0.0) {
        return 0;
    }
    const double order = (attenuation_db - 8.0) / (2.285 * 2.0 * k_pi * transition) + 1.0;
    std::uint64_t taps =
        static_cast<std::uint64_t>(std::ceil(order / static_cast<double>(up)));
    taps = std::max<std::uint64_t>(taps, 8);
    taps += taps & 1u;
    return taps;
}

} // namespace

double plan_cost(std::uint32_t in_rate, std::uint32_t out_rate, const Design& design,
                 const std::vector<Stage>& stages)
{
    double rate = static_cast<double>(in_rate);
    double total = 0.0;
    for (const Stage& stage : stages) {
        rate = rate * stage.up / stage.down;
        const std::uint64_t taps =
            estimate_taps(design.attenuation_db, stage.bandwidth, stage.up, stage.down);
        total += static_cast<double>(taps + 1) * rate;
    }
    return out_rate != 0 ? total / out_rate : total;
}

std::vector<Stage> plan_stages(std::uint32_t in_rate, std::uint32_t out_rate,
                               const Design& design, std::uint32_t max_stages)
{
    const std::uint32_t g = gcd(in_rate, out_rate);
    const std::uint32_t total_up = out_rate / g;
    const std::uint32_t total_down = in_rate / g;

    const Stage single{total_up, total_down, design.bandwidth};
    std::vector<Stage> best{single};
    if (max_stages <= 1 || (total_up == 1 && total_down == 1)) {
        return best;
    }
    double best_cost = plan_cost(in_rate, out_rate, design, best);

    // Everything a stage has to do is fixed by two frequencies: the passband
    // edge the whole conversion keeps, and the lowest rate this stage touches.
    const double lowest = std::min(in_rate, out_rate);
    const double passband_hz = design.bandwidth * lowest / 2.0;
    const double ceiling = 8.0 * std::max(in_rate, out_rate);

    std::vector<Stage> current;
    const auto consider = [&](auto&& self, std::uint32_t up, std::uint32_t down,
                              double rate, std::uint32_t depth) -> void {
        if (up == 1 && down == 1) {
            const double cost = plan_cost(in_rate, out_rate, design, current);
            if (cost < best_cost) {
                best_cost = cost;
                best = current;
            }
            return;
        }
        if (depth == 0) {
            return;
        }
        for (const std::uint32_t u : divisors(up)) {
            for (const std::uint32_t d : divisors(down)) {
                if (u == 1 && d == 1) {
                    continue;
                }
                const std::uint32_t f = gcd(u, d);
                const std::uint32_t su = u / f;
                const std::uint32_t sd = d / f;
                const double next = rate * su / sd;
                // Never below the rate the audio has to survive at, and never
                // absurdly above it either.
                if (next < lowest || next > ceiling) {
                    continue;
                }
                // The band this stage has to protect: everything that could
                // fold into the passband at the lower of its two rates.
                const double lowest_here = std::min(rate, next);
                const double bandwidth = 2.0 * passband_hz / lowest_here;
                if (bandwidth <= 0.0 || bandwidth >= 1.0) {
                    continue;
                }
                const std::uint64_t taps =
                    estimate_taps(design.attenuation_db, bandwidth, su, sd);
                if (taps == 0 || taps * su + 1 > design.max_taps) {
                    continue;
                }
                current.push_back(Stage{su, sd, bandwidth});
                self(self, up / u, down / d, next, depth - 1);
                current.pop_back();
            }
        }
    };
    consider(consider, total_up, total_down, static_cast<double>(in_rate), max_stages);
    return best;
}

// --------------------------------------------------------------------------
// Running one
// --------------------------------------------------------------------------

void Cascade::Buffer::resize(std::uint32_t channels, std::uint32_t frames)
{
    storage.assign(static_cast<std::size_t>(channels) * frames, 0.0);
    planes.resize(channels);
    read.resize(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        planes[c] = storage.data() + static_cast<std::size_t>(c) * frames;
        read[c] = planes[c];
    }
}

bool Cascade::configure(std::uint32_t in_rate, std::uint32_t out_rate,
                        std::uint32_t channels, std::uint32_t max_frames,
                        const Design& design, std::string& why)
{
    if (in_rate == 0 || out_rate == 0 || channels == 0 || max_frames == 0) {
        why = "a rate of zero is not a rate";
        return false;
    }
    channels_ = channels;
    const std::uint32_t g = gcd(in_rate, out_rate);
    up_ = out_rate / g;
    down_ = in_rate / g;

    const std::uint32_t allowed = design.stages == 0 ? 4 : design.stages;
    const std::vector<Stage> plan = plan_stages(in_rate, out_rate, design, allowed);

    stages_.clear();
    stages_.resize(plan.size());
    buffers_.assign(plan.size(), Buffer{});

    double rate = static_cast<double>(in_rate);
    std::uint32_t frames = max_frames;
    multiplies_ = 0.0;
    latency_ = 0.0;
    aligned_ = true;
    response_ = Response{};
    response_.stopband_db = -400.0;

    for (std::size_t i = 0; i < plan.size(); ++i) {
        Design stage_design = design;
        stage_design.bandwidth = plan[i].bandwidth;
        stage_design.stages = 1;
        const auto stage_in = static_cast<std::uint32_t>(std::llround(rate));
        const double next = rate * plan[i].up / plan[i].down;
        const auto stage_out = static_cast<std::uint32_t>(std::llround(next));

        if (!stages_[i].configure(stage_in, stage_out, channels, stage_design, why)) {
            if (plan.size() > 1) {
                why = "stage " + std::to_string(i + 1) + " of " +
                      std::to_string(plan.size()) + " (" + std::to_string(stage_in) +
                      " -> " + std::to_string(stage_out) + "): " + why;
            }
            return false;
        }

        // The delay each stage adds, expressed in the frames the *caller*
        // counts: a stage running at eight times the input rate contributes an
        // eighth as much.
        latency_ += stages_[i].latency_frames() * (in_rate / rate);
        aligned_ = aligned_ && stages_[i].aligned();
        multiplies_ += stages_[i].taps_per_phase() * next / out_rate;
        // A cascade's stopband is the worst of its stages and its passband
        // ripple is at most the sum: an upper bound, and the honest one to
        // report.
        response_.stopband_db =
            std::max(response_.stopband_db, stages_[i].response().stopband_db);
        response_.passband_ripple_db += stages_[i].response().passband_ripple_db;
        response_.points = std::max(response_.points, stages_[i].response().points);

        frames = stages_[i].max_output(frames);
        buffers_[i].resize(channels, frames + 2);
        rate = next;
    }

    pending_.assign(channels, {});
    pending_at_ = 0;
    drained_ = false;
    return true;
}

std::uint32_t Cascade::max_output(std::uint32_t in_frames) const noexcept
{
    std::uint32_t frames = in_frames;
    for (const Resampler& stage : stages_) {
        frames = stage.max_output(frames);
    }
    return frames;
}

bool Cascade::drive(const double* const* in, std::uint32_t in_frames, bool flushing,
                    double* const* out, std::uint32_t capacity, std::uint32_t& produced)
{
    produced = 0;
    const double* const* current = in;
    std::uint32_t frames = in_frames;

    for (std::size_t i = 0; i < stages_.size(); ++i) {
        const auto room = static_cast<std::uint32_t>(buffers_[i].storage.size() / channels_);
        std::uint32_t made = 0;
        const bool ok =
            (flushing && i == 0)
                ? stages_[i].flush(buffers_[i].planes.data(), room, made)
                : stages_[i].process(current, frames, buffers_[i].planes.data(), room, made);
        if (!ok) {
            return false;
        }
        current = buffers_[i].read.data();
        frames = made;
        if (frames == 0) {
            return true;
        }
    }

    if (frames > capacity) {
        return false;
    }
    for (std::uint32_t c = 0; c < channels_; ++c) {
        std::copy_n(current[c], frames, out[c]);
    }
    produced = frames;
    return true;
}

bool Cascade::process(const double* const* in, std::uint32_t in_frames,
                      double* const* out, std::uint32_t capacity, std::uint32_t& produced)
{
    return drive(in, in_frames, false, out, capacity, produced);
}

bool Cascade::flush(double* const* out, std::uint32_t capacity, std::uint32_t& produced)
{
    produced = 0;
    if (stages_.empty()) {
        return true;
    }

    if (!drained_) {
        // Drain the whole cascade once, head first: whatever a stage gives up
        // is ordinary input to the ones behind it, and only when the head is
        // empty does the next one become the head. The result is held here and
        // handed back a block at a time, because the tail is a few hundred
        // frames and the caller's buffer is whatever the device asked for.
        drained_ = true;
        pending_.assign(channels_, {});

        std::vector<double> scratch;
        std::vector<double*> planes(channels_);
        const std::uint32_t room = max_output(1) + 8 +
                                   static_cast<std::uint32_t>(
                                       buffers_.back().storage.size() / channels_);
        scratch.assign(static_cast<std::size_t>(channels_) * room, 0.0);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            planes[c] = scratch.data() + static_cast<std::size_t>(c) * room;
        }

        for (std::size_t head = 0; head < stages_.size(); ++head) {
            for (int round = 0; round < 4096; ++round) {
                const auto space =
                    static_cast<std::uint32_t>(buffers_[head].storage.size() / channels_);
                std::uint32_t made = 0;
                if (!stages_[head].flush(buffers_[head].planes.data(), space, made)) {
                    return false;
                }
                if (made == 0) {
                    break;
                }
                // Push what came out through everything behind it.
                const double* const* current = buffers_[head].read.data();
                std::uint32_t frames = made;
                for (std::size_t i = head + 1; i < stages_.size() && frames != 0; ++i) {
                    const auto space_i =
                        static_cast<std::uint32_t>(buffers_[i].storage.size() / channels_);
                    std::uint32_t next = 0;
                    if (!stages_[i].process(current, frames, buffers_[i].planes.data(),
                                            space_i, next)) {
                        return false;
                    }
                    current = buffers_[i].read.data();
                    frames = next;
                }
                for (std::uint32_t c = 0; c < channels_; ++c) {
                    pending_[c].insert(pending_[c].end(), current[c], current[c] + frames);
                }
            }
        }
    }

    const std::size_t left = pending_.empty() ? 0 : pending_[0].size() - pending_at_;
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(left, capacity));
    for (std::uint32_t c = 0; c < channels_; ++c) {
        std::copy_n(pending_[c].data() + pending_at_, n, out[c]);
    }
    pending_at_ += n;
    produced = n;
    return true;
}

} // namespace mp::resample

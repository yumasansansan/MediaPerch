// SPDX-License-Identifier: GPL-3.0-or-later

#include "convolve.hpp"

#include <transform.hpp>

#include <algorithm>
#include <cmath>

namespace mp::convolve {

bool Convolver::configure(const std::vector<double>& impulse, std::uint32_t channels,
                          std::uint32_t partition, std::string& why)
{
    if (channels == 0 || channels > 64) {
        why = "a convolver needs some channels";
        return false;
    }
    return configure(std::vector<std::vector<double>>(channels, impulse), partition, why);
}

bool Convolver::configure(const std::vector<std::vector<double>>& impulses,
                          std::uint32_t partition, std::string& why)
{
    if (impulses.empty() || impulses.size() > 64) {
        why = "a convolver needs an impulse response for every channel";
        return false;
    }
    std::size_t longest = 0;
    for (const std::vector<double>& impulse : impulses) {
        longest = std::max(longest, impulse.size());
    }
    if (longest == 0) {
        why = "an impulse response of no samples is not one";
        return false;
    }
    // A partition somewhere near a device period is the usual compromise: small
    // enough that the arithmetic is spread evenly across blocks, large enough
    // that the transform is worth doing.
    std::uint32_t want = partition == 0 ? 1024 : partition;
    want = static_cast<std::uint32_t>(mp::transform::next_power_of_two(want));
    if (want < 16 || want > (1u << 20)) {
        why = "that partition size is not one";
        return false;
    }

    partition_ = want;
    transform_ = want * 2;
    bins_ = transform_ / 2 + 1;
    channels_ = static_cast<std::uint32_t>(impulses.size());
    taps_ = longest;
    partitions_ =
        static_cast<std::uint32_t>((longest + partition_ - 1) / partition_);
    plan_ = mp::transform::RealFft(transform_);

    spectra_.assign(static_cast<std::size_t>(channels_) * partitions_ * bins_, {0.0, 0.0});
    std::vector<double> piece(transform_);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const std::vector<double>& impulse = impulses[c];
        for (std::uint32_t p = 0; p < partitions_; ++p) {
            std::fill(piece.begin(), piece.end(), 0.0);
            const std::size_t from = static_cast<std::size_t>(p) * partition_;
            // A channel whose impulse is shorter than the longest simply has
            // zeros there, which is what a shorter impulse means.
            const std::size_t n =
                from >= impulse.size() ? 0
                                       : std::min<std::size_t>(partition_,
                                                               impulse.size() - from);
            std::copy_n(impulse.begin() + static_cast<std::ptrdiff_t>(from), n, piece.begin());
            plan_.forward(piece.data(),
                          spectra_.data() +
                              (static_cast<std::size_t>(c) * partitions_ + p) * bins_);
        }
    }

    reset();
    return true;
}

void Convolver::reset() noexcept
{
    const std::size_t count = partitions();
    history_.assign(static_cast<std::size_t>(channels_) * count * bins_,
                    std::complex<double>{0.0, 0.0});
    tail_.assign(static_cast<std::size_t>(channels_) * partition_, 0.0);
    pending_.assign(static_cast<std::size_t>(channels_) * partition_, 0.0);
    scratch_.assign(transform_, 0.0);
    sum_.assign(bins_, {0.0, 0.0});
    cursor_ = 0;
    held_ = 0;
    taken_ = 0;
    emitted_ = 0;
}

std::uint32_t Convolver::max_output(std::uint32_t frames) const noexcept
{
    // Whatever is held plus what is arriving, rounded down to whole partitions
    // -- which is never more than one partition past the input.
    return frames + partition_;
}

double Convolver::multiplies() const noexcept
{
    if (partition_ == 0) {
        return 0.0;
    }
    const double n = transform_;
    const double b = partition_;
    // Two real transforms of 2B points per B output samples -- each a complex
    // one of B points and a pass to pull it apart -- plus one complex
    // multiply-accumulate per partition per bin, of B + 1 bins. Counted as real
    // multiplies so it can be compared with a biquad's five.
    const double half = n / 2.0;
    return (2.0 * (5.0 * half * std::log2(half) + 4.0 * half) +
            6.0 * partitions() * (half + 1.0)) /
           b;
}

void Convolver::run_block(std::uint32_t channel, double* out) noexcept
{
    const std::size_t count = partitions();
    const std::size_t bins = bins_;
    double* tail = tail_.data() + static_cast<std::size_t>(channel) * partition_;
    const double* fresh = pending_.data() + static_cast<std::size_t>(channel) * partition_;

    // Overlap-save: the transform sees the previous block and this one, and
    // only the second half of what comes back is a true linear convolution.
    // The spectrum goes straight into the delay line.
    std::copy_n(tail, partition_, scratch_.data());
    std::copy_n(fresh, partition_, scratch_.data() + partition_);
    plan_.forward(scratch_.data(),
                  history_.data() + (static_cast<std::size_t>(channel) * count + cursor_) * bins);

    // The multiply-accumulate written out as real arithmetic on the parts,
    // which is a loop the compiler vectorises; a std::complex product carries a
    // branch for the infinities of Annex G in one C library, and a loop with it
    // stays scalar.
    std::fill(sum_.begin(), sum_.end(), std::complex<double>{0.0, 0.0});
    std::complex<double>* sum = sum_.data();
    for (std::size_t p = 0; p < count; ++p) {
        // The block from `p` blocks ago meets the impulse's `p`th partition.
        const std::size_t at = (cursor_ + count - p) % count;
        const std::complex<double>* x =
            history_.data() + (static_cast<std::size_t>(channel) * count + at) * bins;
        const std::complex<double>* h =
            spectra_.data() + (static_cast<std::size_t>(channel) * count + p) * bins;
        for (std::size_t k = 0; k < bins; ++k) {
            const double hr = h[k].real();
            const double hi = h[k].imag();
            const double xr = x[k].real();
            const double xi = x[k].imag();
            sum[k] = {sum[k].real() + (hr * xr - hi * xi), sum[k].imag() + (hr * xi + hi * xr)};
        }
    }
    plan_.inverse(sum_.data(), scratch_.data());

    std::copy_n(scratch_.data() + partition_, partition_, out);
    std::copy_n(fresh, partition_, tail);
}

void Convolver::process(const double* const* in, std::uint32_t frames,
                        double* const* out, std::uint32_t capacity,
                        std::uint32_t& produced) noexcept
{
    produced = 0;
    if (partition_ == 0 || frames == 0 || in == nullptr) {
        return;
    }
    // `max_output` says exactly how much room this needs, and a caller that did
    // not leave it gets nothing rather than a partial answer it cannot tell
    // from a whole one.
    if (capacity < max_output(frames)) {
        return;
    }

    std::uint32_t at = 0;
    while (at < frames) {
        const std::uint32_t take = std::min(partition_ - held_, frames - at);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            std::copy_n(in[c] + at, take,
                        pending_.data() + static_cast<std::size_t>(c) * partition_ + held_);
        }
        held_ += take;
        at += take;
        taken_ += take;

        if (held_ < partition_) {
            break;
        }
        for (std::uint32_t c = 0; c < channels_; ++c) {
            run_block(c, out[c] + produced);
        }
        cursor_ = static_cast<std::uint32_t>((cursor_ + 1) % partitions());
        produced += partition_;
        emitted_ += partition_;
        held_ = 0;
    }
}

void Convolver::flush(double* const* out, std::uint32_t capacity, std::uint32_t& produced)
{
    produced = 0;
    if (partition_ == 0) {
        return;
    }
    // A linear convolution of `n` samples with `taps` of impulse is
    // `n + taps - 1` long, so there is an exact number here rather than a
    // guess: keep going until that many frames have come out, and stop on the
    // sample that number lands on.
    const std::uint64_t target = taken_ + (taps_ > 0 ? taps_ - 1 : 0);
    if (emitted_ >= target) {
        return;
    }

    while (emitted_ < target && produced + partition_ <= capacity) {
        // Silence into whatever the last partition was short of, then a block.
        for (std::uint32_t c = 0; c < channels_; ++c) {
            double* slot = pending_.data() + static_cast<std::size_t>(c) * partition_;
            std::fill(slot + held_, slot + partition_, 0.0);
        }
        held_ = 0;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            run_block(c, out[c] + produced);
        }
        cursor_ = static_cast<std::uint32_t>((cursor_ + 1) % partitions());

        const auto useful = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(partition_, target - emitted_));
        emitted_ += useful;
        produced += useful;
    }
}

} // namespace mp::convolve

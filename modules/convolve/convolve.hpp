// SPDX-License-Identifier: GPL-3.0-or-later
//
// Convolution, partitioned, in the frequency domain.
//
// **Direct convolution is not an implementation choice here, it is a
// non-starter.** A sixty-five-thousand-tap impulse response costs 65,536
// multiply-accumulates per sample per channel; at 96 kHz stereo that is twelve
// billion a second, which is tens of times real time. The transform is not an
// optimisation, it is the difference between a feature and an idea.
//
// The arrangement is uniformly-partitioned overlap-save with a frequency-domain
// delay line. The impulse is cut into `partition`-sample pieces and each is
// transformed once, at configure; the input is transformed once per block and
// kept, and every output block is a sum of products of things already
// transformed. Per sample that is two transforms of 2B points divided by B,
// plus one complex multiply-accumulate per partition -- a few hundred flops
// where the direct form wanted sixty-five thousand.
//
// **It adds no delay of its own.** Output frame *n* is the convolution at frame
// *n*: the block structure means results arrive `partition` at a time rather
// than one at a time, but nothing is shifted. Whatever delay a caller sees is
// the impulse response's own, which for a linear-phase filter is half its
// length and for a minimum-phase one is nearly nothing -- and that is a
// property of the filter, not of this.

#ifndef MEDIAPERCH_CONVOLVE_HPP
#define MEDIAPERCH_CONVOLVE_HPP

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::convolve {

class Convolver {
public:
    /// One impulse response, applied to every channel.
    ///
    /// `partition` is rounded up to a power of two and is the only thing that
    /// trades memory against arithmetic: bigger partitions mean fewer, larger
    /// transforms and more of them held at once. Zero picks one.
    [[nodiscard]] bool configure(const std::vector<double>& impulse,
                                 std::uint32_t channels, std::uint32_t partition,
                                 std::string& why);

    /// One impulse response *per channel*, which is what a stereo room
    /// measurement is: the left ear's response is not the right ear's, and
    /// applying one of them to both is a measurement of half the room.
    ///
    /// They may differ in length; the shorter ones are read as zero past their
    /// end, which is what they are.
    [[nodiscard]] bool configure(const std::vector<std::vector<double>>& impulses,
                                 std::uint32_t partition, std::string& why);

    /// Deinterleaved in, deinterleaved out. **The output is not the same length
    /// as the input**: results arrive a partition at a time, so a call may
    /// produce nothing and the next may produce two blocks. That is what
    /// `MpDspVtbl::process` was shaped to allow.
    void process(const double* const* in, std::uint32_t frames, double* const* out,
                 std::uint32_t capacity, std::uint32_t& produced) noexcept;

    /// The tail: everything the impulse is still ringing with, and the samples
    /// held back because they did not fill a partition. Call until it says
    /// zero.
    void flush(double* const* out, std::uint32_t capacity, std::uint32_t& produced);

    void reset() noexcept;

    /// Room a caller must have for `frames` of input.
    [[nodiscard]] std::uint32_t max_output(std::uint32_t frames) const noexcept;
    [[nodiscard]] std::uint32_t partition() const noexcept { return partition_; }
    [[nodiscard]] std::uint32_t partitions() const noexcept { return partitions_; }
    [[nodiscard]] std::size_t taps() const noexcept { return taps_; }
    /// Multiply-accumulates per output frame per channel, for a report that has
    /// to say what this costs.
    [[nodiscard]] double multiplies() const noexcept;

private:
    /// Transforms one partition's worth of input and produces one partition's
    /// worth of output, for one channel.
    void run_block(std::uint32_t channel, double* out) noexcept;

    std::uint32_t partition_ = 0;
    std::uint32_t partitions_ = 0;
    std::uint32_t transform_ = 0; ///< twice the partition
    std::uint32_t channels_ = 0;
    std::size_t taps_ = 0;

    /// The impulse, one transformed partition after another, per channel.
    std::vector<std::complex<double>> spectra_;
    /// The frequency-domain delay line: `partitions` spectra per channel.
    std::vector<std::complex<double>> history_;
    std::uint32_t cursor_ = 0;

    /// The previous block of input per channel, which overlap-save needs.
    std::vector<double> tail_;
    /// Input that has not filled a partition yet.
    std::vector<double> pending_;
    std::uint32_t held_ = 0;
    /// Frames in and frames out, which is what says when the tail has finished:
    /// a convolution is `taken + taps - 1` long and not one frame more.
    std::uint64_t taken_ = 0;
    std::uint64_t emitted_ = 0;

    std::vector<std::complex<double>> scratch_;
    std::vector<std::complex<double>> sum_;
};

} // namespace mp::convolve

#endif // MEDIAPERCH_CONVOLVE_HPP

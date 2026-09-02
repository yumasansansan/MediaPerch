// SPDX-License-Identifier: GPL-3.0-or-later
//
// A polyphase rational resampler.
//
// **The ratio is rational and reduced.** 44100 -> 48000 is 160/147, and one
// prototype is designed for that pair; each output sample then costs one phase
// of it rather than the whole thing. A rate pair that does not reduce to
// something small (44100 -> 44101 is 44101/44100) would need a prototype of
// millions of taps, and `configure` refuses it by name rather than allocating
// it: an arbitrary-ratio resampler interpolates between phases and is a
// different thing, worth writing on purpose rather than by accident.
//
// How the coefficients are arrived at is [design.hpp](design.hpp), which is
// where the interesting decisions are. Everything here is bookkeeping: where in
// the stream we are, which phase that lands on, and what is still held.
//
// Everything is `double`, because the bus is.

#ifndef MEDIAPERCH_RESAMPLE_HPP
#define MEDIAPERCH_RESAMPLE_HPP

#include "design.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mp::resample {

/// Named settings, because a person choosing a resampler is choosing a
/// trade-off and not a number of taps.
///
/// The attenuations are chosen against the destination rather than against the
/// ear: 120 dB is past 16-bit and past 20-bit, 144 dB is past 24-bit, and
/// `fast` is the one that admits it is for a laptop rather than for a DAC.
/// `extreme` is past what any container can carry and is there because somebody
/// will want to know what the arithmetic can do.
[[nodiscard]] bool design_from_name(const std::string& name, Design& out);
[[nodiscard]] std::string quality_names();

/// The engine: one designed filter, and the state of one stream through it.
class Resampler {
public:
    /// Designs the filter. Returns false and fills `why` when the ratio or the
    /// specification cannot be done, which is a refusal the host must pass on
    /// rather than paper over.
    [[nodiscard]] bool configure(std::uint32_t in_rate, std::uint32_t out_rate,
                                 std::uint32_t channels, const Design& design,
                                 std::string& why);

    /// True when the rates are equal: then this is a copy and nothing is
    /// filtered. The general path would also produce a copy -- at 1:1 the
    /// design really is a unit impulse, and a test asserts it -- but doing the
    /// arithmetic to discover that for every sample would be silly.
    [[nodiscard]] bool identity() const noexcept { return up_ == 1 && down_ == 1; }

    [[nodiscard]] std::uint32_t up() const noexcept { return up_; }
    [[nodiscard]] std::uint32_t down() const noexcept { return down_; }
    /// Multiplies per output sample.
    [[nodiscard]] std::uint32_t taps_per_phase() const noexcept { return phase_taps_; }
    /// The delay that is *left*, in input frames.
    ///
    /// Zero for a linear-phase filter: its delay is exactly half the filter and
    /// is taken out by where the output is sampled from, so output frame k is
    /// input frame k * down / up. A minimum-phase filter has no such number --
    /// its delay depends on frequency -- so what is reported is where the
    /// energy actually sits, and it is not compensated because it cannot be.
    [[nodiscard]] double latency_frames() const noexcept { return latency_; }
    /// Whether the alignment above is exact.
    [[nodiscard]] bool aligned() const noexcept { return !minimum_phase_; }
    /// What the design actually achieved, measured rather than assumed.
    [[nodiscard]] const Response& response() const noexcept { return response_; }

    /// Room a caller must have for `in_frames` of input.
    [[nodiscard]] std::uint32_t max_output(std::uint32_t in_frames) const noexcept;

    /// Deinterleaved in, deinterleaved out. `in` may be null with `in_frames`
    /// zero. Returns false only when asked to write past `capacity`.
    [[nodiscard]] bool process(const double* const* in, std::uint32_t in_frames,
                               double* const* out, std::uint32_t capacity,
                               std::uint32_t& produced);

    /// The end of the stream: everything the filter still holds, and not one
    /// frame more than the ratio says the file is worth. Call until it reports
    /// zero.
    [[nodiscard]] bool flush(double* const* out, std::uint32_t capacity,
                             std::uint32_t& produced);

    void reset() noexcept;

    /// The designed prototype, for a test that wants to look at the filter
    /// rather than at what it did.
    [[nodiscard]] const std::vector<double>& prototype() const noexcept { return proto_; }
    /// Phase `p`, its taps in the order the inner loop reads them.
    [[nodiscard]] const double* phase(std::uint32_t p) const noexcept
    {
        return coef_.data() + static_cast<std::size_t>(p) * phase_taps_;
    }

private:
    /// Produces while there is input for it and room for it, up to `limit`.
    void produce(std::uint64_t limit, double* const* out, std::uint32_t capacity,
                 std::uint32_t& produced);
    /// Forgets input no output can still need.
    void discard();

    std::uint32_t up_ = 1;
    std::uint32_t down_ = 1;
    std::uint32_t channels_ = 0;
    /// The design's own tap count: even, and what the group delay is half of.
    std::uint32_t taps_ = 0;
    /// What the inner loop runs over: `taps_ + 1`. The prototype is odd-length
    /// so that its centre lands on a sample, which puts one extra coefficient in
    /// phase 0; every other phase is padded with a zero so all phases are the
    /// same length and the loop has no special case.
    std::uint32_t phase_taps_ = 1;
    /// Where in the prototype the output is sampled from. Half the filter for
    /// a linear-phase design, which takes its delay out exactly; the front of
    /// it for a minimum-phase one, whose energy is already there.
    std::uint64_t centre_ = 0;
    bool minimum_phase_ = false;
    double latency_ = 0.0;

    /// What the last design was for. Designing is expensive -- seconds, with
    /// `design=refine` on a long prototype -- and a host configures a stage
    /// twice: once to find out what format comes out, and once with the block
    /// size the device settled on. The filter does not depend on the block
    /// size, so the second one is free.
    Design last_{};
    std::uint32_t last_in_rate_ = 0;
    std::uint32_t last_out_rate_ = 0;
    bool designed_ = false;

    Response response_{};
    std::vector<double> proto_; ///< as designed, for inspection
    std::vector<double> coef_;  ///< the same numbers, per phase, reversed

    /// One run of history per channel. Grown once and then reused: the decode
    /// thread may allocate, but there is no reason to make it a habit.
    std::vector<std::vector<double>> hist_;
    std::size_t held_ = 0;      ///< samples in each channel's run
    std::int64_t base_ = 0;     ///< absolute input index of hist[0]
    std::uint64_t out_k_ = 0;   ///< the next output frame
    std::uint64_t taken_ = 0;   ///< input frames accepted so far
    bool flushed_ = false;
};

/// One stage of a plan: the ratio it applies and the specification it is held
/// to. `bandwidth` is what differs -- see `plan_stages`.
struct Stage {
    std::uint32_t up = 1;
    std::uint32_t down = 1;
    double bandwidth = 0.95;
};

/// How to get from `in_rate` to `out_rate` in at most `max_stages` steps, as
/// cheaply as the estimator can see.
///
/// **Where the saving comes from.** Every stage must pass everything up to the
/// final passband edge and must stop everything that would fold into it. For
/// the last stage those two are a hair apart, which is what makes it expensive.
/// For a stage running at a much higher rate they are far apart -- the first
/// thing that could fold in sits near the *intermediate* Nyquist -- so its
/// filter is a fraction of the length. Splitting 10/147 that way turns 2,294
/// multiplies per output frame into about 540.
///
/// Returns a single stage when it cannot do better, so a caller can always use
/// what comes back.
[[nodiscard]] std::vector<Stage> plan_stages(std::uint32_t in_rate, std::uint32_t out_rate,
                                             const Design& design,
                                             std::uint32_t max_stages);

/// Estimated multiply-accumulates per final output frame, per channel.
[[nodiscard]] double plan_cost(std::uint32_t in_rate, std::uint32_t out_rate,
                               const Design& design, const std::vector<Stage>& stages);

/// A cascade of resamplers, which is what a conversion is once it is allowed
/// more than one stage. A plan of one stage is the ordinary case and costs
/// nothing extra.
class Cascade {
public:
    /// `max_frames` is the largest block `process` will be handed, which is
    /// what the intermediate buffers are sized from.
    [[nodiscard]] bool configure(std::uint32_t in_rate, std::uint32_t out_rate,
                                 std::uint32_t channels, std::uint32_t max_frames,
                                 const Design& design, std::string& why);

    [[nodiscard]] bool identity() const noexcept
    {
        return stages_.empty() || (stages_.size() == 1 && stages_[0].identity());
    }
    [[nodiscard]] std::size_t size() const noexcept { return stages_.size(); }
    [[nodiscard]] const Resampler& stage(std::size_t i) const noexcept
    {
        return stages_[i];
    }

    [[nodiscard]] std::uint32_t up() const noexcept { return up_; }
    [[nodiscard]] std::uint32_t down() const noexcept { return down_; }
    /// Multiplies per output frame, summed over the stages at the rates they
    /// actually run at.
    [[nodiscard]] double multiplies() const noexcept { return multiplies_; }
    /// The worst stopband of any stage, and the sum of the passband ripples --
    /// which is what a cascade's response is bounded by.
    [[nodiscard]] const Response& response() const noexcept { return response_; }
    [[nodiscard]] double latency_frames() const noexcept { return latency_; }
    [[nodiscard]] bool aligned() const noexcept { return aligned_; }

    [[nodiscard]] std::uint32_t max_output(std::uint32_t in_frames) const noexcept;

    [[nodiscard]] bool process(const double* const* in, std::uint32_t in_frames,
                               double* const* out, std::uint32_t capacity,
                               std::uint32_t& produced);
    [[nodiscard]] bool flush(double* const* out, std::uint32_t capacity,
                             std::uint32_t& produced);

private:
    /// Buffers for one stage's output, one plane per channel.
    struct Buffer {
        std::vector<double> storage;
        std::vector<double*> planes;
        std::vector<const double*> read;
        void resize(std::uint32_t channels, std::uint32_t frames);
    };

    [[nodiscard]] bool drive(const double* const* in, std::uint32_t in_frames,
                             bool flushing, double* const* out, std::uint32_t capacity,
                             std::uint32_t& produced);

    std::vector<Resampler> stages_;
    std::vector<Buffer> buffers_;
    std::uint32_t channels_ = 0;
    std::uint32_t up_ = 1;
    std::uint32_t down_ = 1;
    double multiplies_ = 0.0;
    double latency_ = 0.0;
    bool aligned_ = true;
    Response response_{};
    /// What a full drain produced, handed back a block at a time.
    std::vector<std::vector<double>> pending_;
    std::size_t pending_at_ = 0;
    bool drained_ = false;
};

} // namespace mp::resample

#endif // MEDIAPERCH_RESAMPLE_HPP

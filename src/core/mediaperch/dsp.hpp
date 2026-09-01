// SPDX-License-Identifier: GPL-3.0-or-later
//
// Path B's chain: the place where things are meant to touch the samples.
//
// Path A exists so a file can reach a device with nothing between them. This is
// the other side of that decision -- a resampler, a ReplayGain, an equaliser, a
// convolver are all the same shape of thing, and the shape is `MpDspVtbl`.
//
// The bus is deinterleaved `double`, one contiguous array per channel, because
// that is what a filter wants and because the conversion at either end of this
// chain already works in binary64. Putting an f32 bus in the middle would add a
// rounding to the one path in this program whose argument is that it has
// exactly one.
//
// Everything here runs on the decode thread. The render thread still copies
// bytes out of a ring and knows nothing, which is §2's rule and is why the two
// graphs are two graphs.

#ifndef MEDIAPERCH_DSP_HPP
#define MEDIAPERCH_DSP_HPP

#include "mediaperch/format.hpp"

#include <mediaperch/module.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mp {

/// The chain's own format: a format with its samples as 64-bit floats.
///
/// Everything between the two converters of Path B is in this format, so the
/// widening at the front is exact and the quantiser at the back is the only
/// one. `valid_bits` goes to zero because it describes an integer container
/// and means nothing here.
[[nodiscard]] Format dsp_bus_format(const Format& source) noexcept;

/// One loaded stage, and the buffers it writes into.
///
/// Owning the buffers here rather than in the chain keeps the rate-changing
/// case honest: a stage that upsamples produces more frames than it was given,
/// so each stage's output has to be sized from its own answer to `configure`
/// rather than from the block size the chain started with.
class DspStage {
public:
    DspStage(const MpDspVtbl& vtbl, std::string name) noexcept;
    ~DspStage();
    DspStage(const DspStage&) = delete;
    DspStage& operator=(const DspStage&) = delete;
    DspStage(DspStage&&) noexcept;
    DspStage& operator=(DspStage&&) = delete;

    [[nodiscard]] bool open() noexcept;
    /// `key=value`, the form a command line and a settings tree both produce.
    [[nodiscard]] MpResult set(const std::string& key, const std::string& value) noexcept;
    /// Every setting this stage has, one `key\tcurrent\tdescription` per entry.
    [[nodiscard]] std::vector<std::string> describe() const;

    /// What this stage produces from `in`, and how much room its output needs.
    [[nodiscard]] MpResult configure(const Format& in, std::uint32_t max_frames,
                                     Format& out, std::uint32_t& out_max) noexcept;

    /// Runs the stage. `in` may be empty to drain what it already holds.
    [[nodiscard]] MpResult process(const double* const* in, std::uint32_t in_frames,
                                   std::uint32_t& out_frames) noexcept;
    [[nodiscard]] MpResult flush(std::uint32_t& out_frames) noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const Format& output_format() const noexcept { return output_; }
    /// Channel pointers into this stage's own output, for the next stage.
    [[nodiscard]] const double* const* output() const noexcept { return planes_.data(); }

private:
    const MpDspVtbl* vtbl_;
    MpDsp* handle_ = nullptr;
    std::string name_;
    Format output_{};
    std::uint32_t capacity_ = 0;
    std::vector<double> storage_;
    std::vector<double*> planes_;
};

/// The chain, and the conversion at each end of it.
///
/// `input` is the source's format; `output()` is what comes out after every
/// stage has had its say, which is what negotiation must then offer the device.
/// An empty chain is not a special case: it reports the input unchanged and
/// copies.
class DspChain {
public:
    DspChain() = default;

    /// Adds a stage. The chain does not own the vtable, which belongs to a
    /// loaded module and outlives it.
    void add(const MpDspVtbl& vtbl, std::string name);

    [[nodiscard]] bool empty() const noexcept { return stages_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return stages_.size(); }
    [[nodiscard]] DspStage& at(std::size_t i) noexcept { return *stages_[i]; }

    /// Opens every stage and chains their formats. `why` names the stage that
    /// refused, because "the DSP chain would not configure" is not actionable.
    [[nodiscard]] bool configure(const Format& input, std::uint32_t max_frames,
                                 std::string& why);

    /// What was handed to `configure`, so a caller can check it is feeding the
    /// chain what the chain was set up for.
    [[nodiscard]] const Format& input_format() const noexcept { return input_; }
    [[nodiscard]] const Format& output_format() const noexcept { return output_; }
    /// The largest number of frames `run` can produce from one `max_frames`
    /// block, which is what the caller has to size its own buffer from.
    [[nodiscard]] std::uint32_t output_capacity() const noexcept { return capacity_; }

    /// Interleaved `double` in, interleaved `double` out, deinterleaving at the
    /// front and interleaving at the back so the stages see planes.
    ///
    /// Returns false on a stage error. `out` is resized as needed, which is an
    /// allocation the decode thread is allowed to make and the render thread
    /// never sees.
    [[nodiscard]] bool run(const double* interleaved, std::uint32_t frames,
                           std::vector<double>& out, std::uint32_t& out_frames);

    /// Drains the chain at the end of the stream, one round per call, until
    /// `flush_done()`.
    ///
    /// The ABI says a stage is flushed until it reports zero frames, and a
    /// chain is drained head first: whatever the head gives up is ordinary
    /// input to everything behind it, and only when the head is dry does the
    /// stage behind it become the head. One round per call rather than one
    /// drain per call because the caller has a fixed amount of room to put the
    /// result in, and a filter's tail is not a size this class gets to choose.
    [[nodiscard]] bool flush(std::vector<double>& out, std::uint32_t& out_frames);
    [[nodiscard]] bool flush_done() const noexcept { return flush_done_; }

private:
    /// Runs stages `from` onwards over `input`, and interleaves what comes out.
    [[nodiscard]] bool push(std::size_t from, const double* const* input,
                            std::uint32_t frames, std::vector<double>& out,
                            std::uint32_t& out_frames);

    std::vector<std::unique_ptr<DspStage>> stages_;
    Format input_{};
    Format output_{};
    std::uint32_t capacity_ = 0;
    /// Which stage `flush` is draining. Past the end means there is no more.
    std::size_t flush_stage_ = 0;
    bool flush_done_ = false;
    /// The deinterleaved copy of what the caller handed in.
    std::vector<double> scratch_;
    std::vector<double*> scratch_planes_;
};

} // namespace mp

#endif // MEDIAPERCH_DSP_HPP

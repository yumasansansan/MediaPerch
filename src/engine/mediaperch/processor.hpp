// SPDX-License-Identifier: GPL-3.0-or-later
//
// Path B's arithmetic, with no device, no ring and no thread.
//
// **This is the half of `ProcessedGraph` that touches samples**, and it was
// taken out of it for a reason that is not tidiness: there was no way to run
// the DSP chain without a sound card. `mediaperch-probe decode` accepted
// `--path processed` and `--dsp` and silently ignored both -- a −6 dB gain and
// a resample to 192 kHz left the hash exactly where it was -- so every claim
// this project makes about Path B rested on listening to it rather than on
// hashing it, and a build that changed the resampler's output by a bit would
// have passed every test in the tree.
//
// The shape is the one `ProcessedGraph` documents: source → the f64 bus → the
// chain → the wire format. The first step is a widening and therefore exact;
// the last is the only quantiser in Path B, which is what lets the dither and
// the noise shaping sit in one place. Without a chain it is one conversion and
// the bus never exists.
//
// Nothing here allocates after construction, because `ProcessedGraph` calls it
// on the decode thread where that is merely rude, and because a second caller
// will want the same promise.

#ifndef MEDIAPERCH_PROCESSOR_HPP
#define MEDIAPERCH_PROCESSOR_HPP

#include "mediaperch/convert.hpp"
#include "mediaperch/dsp.hpp"
#include "mediaperch/format.hpp"

#include <cstdint>
#include <vector>

namespace mp {

class Processor {
public:
    /// `chain` may be null, and usually is. When it is given it must already be
    /// configured for `dsp_bus_format(source)`, which `possible()` checks:
    /// a chain configured for something other than what it is about to be
    /// handed is a bug in the caller, and one that would otherwise show up as
    /// noise rather than as an error.
    Processor(const Format& source, const Format& wire, std::uint32_t chunk_frames,
              ConvertConfig convert = {}, DspChain* chain = nullptr);

    /// Whether this can run at all: the conversions exist, the formats have
    /// sizes, and the chain was configured for the bus it will be fed.
    [[nodiscard]] bool possible() const noexcept;

    /// Bytes of source one `run` takes, and bytes of wire one may produce.
    ///
    /// The second is not the first converted: a chain that resamples upwards
    /// gives back more frames than it was handed, and `output_capacity` is the
    /// most it ever will.
    [[nodiscard]] std::uint32_t input_bytes() const noexcept { return input_bytes_; }
    [[nodiscard]] std::uint32_t output_bytes() const noexcept { return output_bytes_; }
    [[nodiscard]] std::uint32_t wire_frame_bytes() const noexcept { return wire_frame_bytes_; }
    [[nodiscard]] std::uint32_t source_frame_bytes() const noexcept
    {
        return source_frame_bytes_;
    }

    /// `frames` of source at `src` into `dst`, which must hold `output_bytes()`.
    /// `out_frames` is what came out, and **0 is normal**: a stage may still be
    /// filling its history. False only when the chain failed.
    [[nodiscard]] bool run(const void* src, std::uint32_t frames, void* dst,
                           std::uint32_t& out_frames);

    /// What the chain still holds once the source is finished. A resampler
    /// keeps a filter's worth of the last audio and it is as much the file as
    /// the rest. Call until `flush_done()`; without a chain there is nothing to
    /// drain and this says so at once.
    [[nodiscard]] bool flush(void* dst, std::uint32_t& out_frames);
    [[nodiscard]] bool flush_done() const noexcept { return flushed_; }

    /// Whether the one quantiser at the end is throwing anything away. A
    /// conversion that is not lossy is one where the only change was a gain of
    /// exactly one -- worth being able to say, because "processed" and
    /// "altered" are not the same word.
    [[nodiscard]] bool lossy() const noexcept { return converter_.lossy(); }

    /// The chain's latency, in frames of the bus it runs on. Zero without a
    /// chain. §8 needs it: what is audible is this far behind what the device
    /// position says, because the frames leaving the endpoint went through
    /// the chain and a linear-phase stage delayed them.
    [[nodiscard]] std::uint32_t latency_frames() const noexcept;

    /// Forgets the chain's history, for a seek, and puts the drain back so
    /// the end of the stream is found again. False when a stage refused --
    /// which the graph ignores, because a chain that would not forget is still
    /// a chain that has to keep playing.
    bool reset() noexcept;

private:
    Format source_{};
    Format wire_{};
    Format bus_{};
    /// Without a chain: source straight to the wire format.
    /// With one: the chain's output to the wire format.
    Converter converter_;
    /// Widening only: exact by construction, and nothing to dither because
    /// nothing is being thrown away.
    Converter to_bus_;
    DspChain* chain_ = nullptr;
    std::vector<std::uint8_t> bus_chunk_;
    std::vector<double> chain_out_;
    std::uint32_t chunk_frames_ = 0;
    std::uint32_t source_frame_bytes_ = 0;
    std::uint32_t wire_frame_bytes_ = 0;
    std::uint32_t input_bytes_ = 0;
    std::uint32_t output_bytes_ = 0;
    bool flushed_ = false;

    void emit(std::uint32_t frames, void* dst);
};

} // namespace mp

#endif // MEDIAPERCH_PROCESSOR_HPP

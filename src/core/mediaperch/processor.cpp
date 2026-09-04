// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/processor.hpp"

#include <algorithm>

namespace mp {

Processor::Processor(const Format& source, const Format& wire, std::uint32_t chunk_frames,
                     ConvertConfig convert, DspChain* chain)
    : source_(source), wire_(wire), bus_(dsp_bus_format(source)),
      // The quantiser reads whatever reaches it: the source itself, or the
      // chain's output, which may be at a rate the source never had.
      converter_(chain != nullptr ? chain->output_format() : source, wire, convert),
      to_bus_(source, dsp_bus_format(source), ConvertConfig{}), chain_(chain),
      chunk_frames_(chunk_frames), source_frame_bytes_(frame_bytes(source)),
      wire_frame_bytes_(frame_bytes(wire))
{
    input_bytes_ = chunk_frames_ * source_frame_bytes_;
    // A chain that upsamples gives back more frames than it took, so the room
    // one round needs is its capacity rather than the chunk it was handed.
    const std::uint32_t out_frames =
        chain_ != nullptr ? std::max(chain_->output_capacity(), chunk_frames_) : chunk_frames_;
    output_bytes_ = out_frames * wire_frame_bytes_;
    if (chain_ != nullptr) {
        bus_chunk_.resize(static_cast<std::size_t>(chunk_frames_) * frame_bytes(bus_));
        chain_out_.resize(static_cast<std::size_t>(out_frames) * bus_.channels);
    }
}

bool Processor::possible() const noexcept
{
    if (!converter_.possible() || source_frame_bytes_ == 0 || wire_frame_bytes_ == 0 ||
        chunk_frames_ == 0) {
        return false;
    }
    if (chain_ == nullptr) {
        return true;
    }
    return to_bus_.possible() && chain_->input_format() == bus_ &&
           chain_->output_capacity() != 0;
}

void Processor::emit(std::uint32_t frames, void* dst)
{
    if (frames == 0) {
        return;
    }
    converter_.run(chain_out_.data(), dst, frames);
}

bool Processor::run(const void* src, std::uint32_t frames, void* dst,
                    std::uint32_t& out_frames)
{
    out_frames = 0;
    if (frames == 0) {
        return true;
    }
    if (chain_ == nullptr) {
        // The common case: one conversion, and no bus at all.
        converter_.run(src, dst, frames);
        out_frames = frames;
        return true;
    }
    to_bus_.run(src, bus_chunk_.data(), frames);
    std::uint32_t produced = 0;
    if (!chain_->run(reinterpret_cast<const double*>(bus_chunk_.data()), frames, chain_out_,
                     produced)) {
        return false;
    }
    emit(produced, dst); // 0 is normal: a stage may still be filling its history
    out_frames = produced;
    return true;
}

bool Processor::flush(void* dst, std::uint32_t& out_frames)
{
    out_frames = 0;
    if (chain_ == nullptr || flushed_) {
        flushed_ = true;
        return false; // nothing was held back, so there is nothing to give back
    }
    std::uint32_t produced = 0;
    if (!chain_->flush(chain_out_, produced)) {
        flushed_ = true;
        return false;
    }
    emit(produced, dst);
    out_frames = produced;
    flushed_ = chain_->flush_done();
    // Coming back for another round counts as having done something, even when
    // this round produced nothing: the stage behind this one may still be full.
    return !flushed_ || produced != 0;
}

bool Processor::reset() noexcept
{
    flushed_ = false;
    // The quantiser too: its shaper is a feedback loop over samples that are no
    // longer the ones before these.
    converter_.reset();
    return chain_ == nullptr || chain_->reset();
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/dsp.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mp {
namespace {

/// Points `planes` at `channels` contiguous runs of `capacity` doubles inside
/// `storage`, resizing it. Deinterleaved means exactly this and nothing else.
void lay_out(std::vector<double>& storage, std::vector<double*>& planes,
             std::uint32_t channels, std::uint32_t capacity)
{
    storage.assign(static_cast<std::size_t>(channels) * capacity, 0.0);
    planes.resize(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        planes[c] = storage.data() + static_cast<std::size_t>(c) * capacity;
    }
}

} // namespace

Format dsp_bus_format(const Format& source) noexcept
{
    Format out = source;
    out.sample_type = SampleType::f64;
    out.valid_bits = 0;
    return out;
}

DspStage::DspStage(const MpDspVtbl& vtbl, std::string name) noexcept
    : vtbl_(&vtbl), name_(std::move(name))
{
}

DspStage::DspStage(DspStage&& other) noexcept
    : vtbl_(other.vtbl_), handle_(other.handle_), name_(std::move(other.name_)),
      output_(other.output_), capacity_(other.capacity_),
      storage_(std::move(other.storage_)), planes_(std::move(other.planes_))
{
    other.handle_ = nullptr;
}

DspStage::~DspStage()
{
    if (handle_ != nullptr && vtbl_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
}

bool DspStage::open() noexcept
{
    if (handle_ != nullptr) {
        return true;
    }
    // Big enough for the calls this host actually makes, rather than for the
    // header it happened to be compiled against: a vtable grows at the end, and
    // demanding the newest size would refuse every module built before it.
    constexpr std::uint32_t k_needed = offsetof(MpDspVtbl, describe) + sizeof(void*);
    if (vtbl_ == nullptr || vtbl_->size < k_needed || vtbl_->open == nullptr ||
        vtbl_->configure == nullptr || vtbl_->process == nullptr) {
        return false;
    }
    return vtbl_->open(&handle_) == MP_OK && handle_ != nullptr;
}

MpResult DspStage::set(const std::string& key, const std::string& value) noexcept
{
    if (handle_ == nullptr || vtbl_->set == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->set(handle_, key.c_str(), value.c_str());
}

std::vector<std::string> DspStage::describe() const
{
    std::vector<std::string> out;
    if (handle_ == nullptr || vtbl_->describe == nullptr) {
        return out;
    }
    char line[512];
    for (std::uint32_t i = 0; i < 64; ++i) {
        line[0] = '\0';
        if (vtbl_->describe(handle_, i, line, sizeof(line)) != MP_OK) {
            break;
        }
        out.emplace_back(line);
    }
    return out;
}

MpResult DspStage::configure(const Format& in, std::uint32_t max_frames, Format& out,
                             std::uint32_t& out_max) noexcept
{
    if (handle_ == nullptr) {
        return MP_ERR_INVALID;
    }
    const MpFormat want = mp::to_abi(in);
    MpFormat produced{};
    std::uint32_t produced_frames = 0;
    const MpResult r =
        vtbl_->configure(handle_, &want, max_frames, &produced, &produced_frames);
    if (r != MP_OK) {
        return r;
    }
    output_ = mp::from_abi(produced);
    if (!is_valid(output_) || produced_frames == 0) {
        return MP_ERR_FORMAT;
    }
    capacity_ = produced_frames;
    lay_out(storage_, planes_, output_.channels, capacity_);
    out = output_;
    out_max = capacity_;
    return MP_OK;
}

MpResult DspStage::process(const double* const* in, std::uint32_t in_frames,
                           std::uint32_t& out_frames) noexcept
{
    out_frames = 0;
    if (handle_ == nullptr) {
        return MP_ERR_INVALID;
    }
    return vtbl_->process(handle_, in, in_frames, planes_.data(), capacity_, &out_frames);
}

MpResult DspStage::flush(std::uint32_t& out_frames) noexcept
{
    out_frames = 0;
    if (handle_ == nullptr) {
        return MP_ERR_INVALID;
    }
    if (vtbl_->flush == nullptr) {
        return MP_OK; // a stage with no memory has nothing to drain
    }
    return vtbl_->flush(handle_, planes_.data(), capacity_, &out_frames);
}

// --------------------------------------------------------------------------

MpResult DspStage::reset() noexcept
{
    if (handle_ == nullptr) {
        return MP_ERR_INVALID;
    }
    // A vtable that stops before `reset` belongs to a module built against the
    // older header. Nothing is wrong with it; it simply cannot be told.
    if (vtbl_->size < offsetof(MpDspVtbl, reset) + sizeof(void*) ||
        vtbl_->reset == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->reset(handle_);
}

std::uint32_t DspStage::latency_frames() const noexcept
{
    // Same reasoning as `reset`: a vtable that stops before this belongs to a
    // module built against the older header. It has a latency; it simply cannot
    // be asked, and reporting 0 is the only answer available.
    if (handle_ == nullptr ||
        vtbl_->size < offsetof(MpDspVtbl, get_latency) + sizeof(void*) ||
        vtbl_->get_latency == nullptr) {
        return 0;
    }
    std::uint32_t frames = 0;
    return vtbl_->get_latency(handle_, &frames) == MP_OK ? frames : 0;
}

std::uint32_t DspChain::latency_frames() const noexcept
{
    // Added rather than maximised: they are in series, so each one's delay is
    // paid after the one before it.
    std::uint32_t total = 0;
    for (const auto& stage : stages_) {
        total += stage->latency_frames();
    }
    return total;
}

void DspChain::add(const MpDspVtbl& vtbl, std::string name)
{
    stages_.push_back(std::make_unique<DspStage>(vtbl, std::move(name)));
}

bool DspChain::configure(const Format& input, std::uint32_t max_frames, std::string& why)
{
    input_ = input;
    output_ = input;
    capacity_ = max_frames;
    lay_out(scratch_, scratch_planes_, input.channels, max_frames);

    Format running = input;
    std::uint32_t frames = max_frames;
    for (auto& stage : stages_) {
        if (!stage->open()) {
            why = stage->name() + " would not open";
            return false;
        }
        Format produced{};
        std::uint32_t produced_frames = 0;
        if (stage->configure(running, frames, produced, produced_frames) != MP_OK) {
            // Naming the stage matters: a chain of four that will not configure
            // is four things to look at, and three of them are innocent.
            why = stage->name() + " refused " + describe(running);
            // And a stage that knows more than `MP_ERR_FORMAT` gets to say it.
            // The ABI has no error string -- a vtable that returned one would
            // have to own it -- so the convention is a `trouble` line in
            // `describe`, which a stage already has somewhere to put. Without
            // it, `dsp_vst3` refusing because a file is not there reads as a
            // format it did not like.
            for (const std::string& line : stage->describe()) {
                if (line.rfind("trouble\t", 0) != 0) {
                    continue;
                }
                const std::size_t from = line.find('\t') + 1;
                const std::size_t to = line.find('\t', from);
                const std::string what = line.substr(from, to - from);
                if (!what.empty() && what != "nothing") {
                    why += ": " + what;
                }
            }
            return false;
        }
        running = produced;
        frames = produced_frames;
    }
    output_ = running;
    capacity_ = frames;
    flush_stage_ = 0;
    flush_done_ = stages_.empty();
    return true;
}

bool DspChain::push(std::size_t from, const double* const* input, std::uint32_t frames,
                    std::vector<double>& out, std::uint32_t& out_frames)
{
    out_frames = 0;

    const double* const* current = input;
    std::uint32_t current_frames = frames;

    for (std::size_t i = from; i < stages_.size(); ++i) {
        DspStage& stage = *stages_[i];
        std::uint32_t produced = 0;
        if (stage.process(current, current_frames, produced) != MP_OK) {
            return false;
        }
        current = stage.output();
        current_frames = produced;
        if (current_frames == 0) {
            break; // nothing for anything downstream to do this block
        }
    }

    // Back to interleaved for the quantiser at the end of Path B.
    const std::uint32_t channels = output_.channels;
    out.resize(static_cast<std::size_t>(current_frames) * channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        const double* plane = current[c];
        for (std::uint32_t n = 0; n < current_frames; ++n) {
            out[static_cast<std::size_t>(n) * channels + c] = plane[n];
        }
    }
    out_frames = current_frames;
    return true;
}

bool DspChain::run(const double* interleaved, std::uint32_t frames, std::vector<double>& out,
                   std::uint32_t& out_frames)
{
    out_frames = 0;
    if (frames == 0) {
        return true;
    }

    if (stages_.empty()) {
        // No stages: the chain is a copy, and saying so here keeps every caller
        // from having to know whether there are any.
        out.assign(interleaved,
                   interleaved + static_cast<std::size_t>(frames) * input_.channels);
        out_frames = frames;
        return true;
    }

    const std::uint32_t channels = input_.channels;
    for (std::uint32_t c = 0; c < channels; ++c) {
        double* plane = scratch_planes_[c];
        for (std::uint32_t n = 0; n < frames; ++n) {
            plane[n] = interleaved[static_cast<std::size_t>(n) * channels + c];
        }
    }
    return push(0, scratch_planes_.data(), frames, out, out_frames);
}

bool DspChain::reset()
{
    bool clean = true;
    for (auto& stage : stages_) {
        const MpResult r = stage->reset();
        // A stage with no memory has nothing to drop, and saying so is not a
        // failure.
        clean = clean && (r == MP_OK || r == MP_ERR_UNSUPPORTED);
    }
    flush_stage_ = 0;
    flush_done_ = stages_.empty();
    return clean;
}

bool DspChain::flush(std::vector<double>& out, std::uint32_t& out_frames)
{
    out_frames = 0;
    if (flush_stage_ >= stages_.size()) {
        flush_done_ = true;
        return true;
    }

    std::uint32_t produced = 0;
    if (stages_[flush_stage_]->flush(produced) != MP_OK) {
        flush_done_ = true;
        return false;
    }
    if (produced == 0) {
        // That one is empty. The stage behind it becomes the head, and is
        // drained the same way on the next round.
        ++flush_stage_;
        flush_done_ = flush_stage_ >= stages_.size();
        return true;
    }
    return push(flush_stage_ + 1, stages_[flush_stage_]->output(), produced, out,
                out_frames);
}

} // namespace mp

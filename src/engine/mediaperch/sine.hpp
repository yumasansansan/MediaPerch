// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/source.hpp"

#include <cstdint>

namespace mp {

/// An endless tone, generated straight into the wire format.
///
/// Milestone 1's source: it lets the whole audio path -- negotiation, the ring,
/// the render thread, the device -- be exercised before any decoder exists, and
/// it is the only source whose correct output can be written down in closed
/// form, which makes it the one to reach for when something sounds wrong.
///
/// Deliberately integer-only. A float bus is Path B, and Path B is not this
/// milestone.
class SineSource final : public ISource {
public:
    /// `amplitude` is a fraction of full scale; the default leaves headroom so
    /// that a rounding mistake shows up as distortion rather than as clipping
    /// that could have come from anywhere.
    SineSource(const Format& format, double hz, double amplitude = 0.5);

    [[nodiscard]] const Format& format() const noexcept override { return format_; }

    /// Always fills `bytes` rounded down to a whole frame. Never returns 0: a
    /// tone has no end.
    std::size_t read(void* dst, std::size_t bytes) override;

    /// Frames produced so far. The phase is derived from this rather than
    /// accumulated, so a long run cannot drift.
    [[nodiscard]] std::uint64_t frames_produced() const noexcept { return frame_; }

private:
    void write_frame(std::uint8_t* dst, std::int32_t value) const noexcept;

    Format format_;
    double hz_;
    double amplitude_;
    std::uint64_t frame_ = 0;
    /// Cycle length in frames when the tone divides the rate exactly, so the
    /// phase can be reduced without ever growing a large argument.
    std::uint64_t period_frames_ = 0;
};

} // namespace mp

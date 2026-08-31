// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <vector>

namespace mp {

/// How faithfully a format the sink accepted carries the source.
///
/// The distinction between `exact` and `widened` is not cosmetic: `exact` is one
/// `memcpy`, `widened` is a fixed integer left-shift per sample. Both are
/// bit-exact -- no signal is lost, no gain applied, no rate changed -- but only
/// the first is literally a copy, and the passthrough graph has to know which it
/// is doing.
enum class Fidelity : std::uint32_t {
    exact,     ///< byte-identical. `memcpy`.
    widened,   ///< a wider container holding the same bits. A shift, no float.
    converted, ///< anything else. Path B, and a user decision -- never produced here.
};

struct Candidate {
    Format format;
    Fidelity fidelity = Fidelity::exact;
    /// The source did not name a channel layout and this candidate does. Some
    /// drivers accept only the extensible form, even for stereo.
    bool channel_mask_added = false;

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

/// True for the fidelities the passthrough graph may serve.
[[nodiscard]] constexpr bool is_bit_exact(Fidelity f) noexcept
{
    return f == Fidelity::exact || f == Fidelity::widened;
}

/// The candidate list, in the order a sink should be asked -- and it stops
/// deliberately early.
///
/// Order is: for each container from the source's own outwards, the plain form
/// first and the extensible form second. Trying every extensible variant only
/// after every widening (which is how it reads if you list the rules as prose)
/// would widen a format needlessly on any driver that simply wants a channel
/// mask, so the mask variant is paired with its base rather than appended.
///
/// Nothing past a widening is offered. A rate change, a channel change or a
/// narrowing is a conversion, and a conversion is Path B and the user's call.
///
/// Non-PCM encodings get no widening at all: shifting a DoP frame moves the
/// marker bits, and a bitstream is not samples.
///
/// Returns empty for a format `is_valid` rejects.
[[nodiscard]] std::vector<Candidate> build_candidates(const Format& source);

/// What the sink actually gave us, against what we asked for.
[[nodiscard]] Fidelity classify(const Format& source, const Format& accepted) noexcept;

} // namespace mp

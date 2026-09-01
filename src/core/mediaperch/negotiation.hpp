// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <string_view>
#include <vector>

namespace mp {

/// How faithfully a format the sink accepted carries the source.
///
/// The distinction between `exact` and `repacked` is not cosmetic: `exact` is
/// one `memcpy`, `repacked` moves every sample into a different container. Both
/// are bit-exact -- no signal is lost, no gain applied, no rate changed -- but
/// only the first is literally a copy, and the passthrough graph has to know
/// which it is doing.
enum class Fidelity : std::uint32_t {
    exact,     ///< the same container and the same valid bits. `memcpy`.
    repacked,  ///< the same bits in a different container. No float, no rounding.
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

/// Which graph a stream is allowed to take.
///
/// Until now this was not a choice: the only graph that exists is the bit-exact
/// one, so negotiation offered every container that could carry the signal and
/// refused the file if none of them stuck. That is the right default and it
/// stays the default. It is not the right *only* behaviour, for two reasons a
/// user can have today and one that arrives with the processed graph:
///
///   * a container repack is bit-exact and is still not a `memcpy`. Somebody
///     who wants to know that nothing at all touched the samples -- not even a
///     byte move -- can now ask for that and be refused rather than served
///     quietly by the second-best answer.
///   * negotiating costs a real `IAudioClient::Initialize` per candidate, and
///     in exclusive mode each one takes the device. Asking for fewer candidates
///     is a thing a user may reasonably want.
///   * `processed` is here so the setting has the shape it will need, and it
///     refuses honestly instead of pretending. Nothing in this tree implements
///     Path B yet.
enum class PathPolicy : std::uint32_t {
    automatic,  ///< bit-exact if the device will take it. The default.
    exact_only, ///< one memcpy or nothing: a container repack is refused too.
    processed,  ///< always the processed graph. Not built yet, and says so.
};

/// The name a user types, or nullptr for a word that is not one of them.
[[nodiscard]] const char* path_policy_name(PathPolicy p) noexcept;
[[nodiscard]] bool path_policy_from_name(std::string_view name, PathPolicy& out) noexcept;

/// True for the fidelities the passthrough graph may serve.
[[nodiscard]] constexpr bool is_bit_exact(Fidelity f) noexcept
{
    return f == Fidelity::exact || f == Fidelity::repacked;
}

/// The candidate list, in the order a sink should be asked -- and it stops
/// deliberately early.
///
/// Candidates are generated over **containers**, not over sample types. Every
/// container that can hold the source's valid bits is offered: the source's own
/// first, then the others from small to large. That includes containers
/// *smaller* than the source's, because "24-bit" names two different wire
/// formats -- three bytes packed, and 24 valid bits inside four -- and real
/// devices want one or the other. A virtual cable configured for 24-bit wanted
/// the three-byte form; the machine's onboard codec wanted the four-byte form.
/// Offering only one of them means refusing perfectly playable audio.
///
/// For each container the plain form comes first and the extensible form with a
/// channel mask second. Trying every extensible variant only after every other
/// container -- which is how it reads if the rules are listed as prose -- would
/// change the sample container needlessly on any driver whose only complaint was
/// the missing channel mask, and a real device does exactly that.
///
/// Nothing beyond a repack is offered. A rate change, a channel change or losing
/// valid bits is a conversion, and a conversion is Path B and the user's call.
///
/// Non-PCM encodings get no repack at all: moving a DoP frame between containers
/// shifts its marker bits, and a bitstream is not samples.
///
/// Returns empty for a format `is_valid` rejects, and for `PathPolicy::processed`,
/// which has no candidates because it has no graph.
[[nodiscard]] std::vector<Candidate> build_candidates(const Format& source,
                                                      PathPolicy policy = PathPolicy::automatic);

/// True when `policy` allows a stream that reached `fidelity` to play.
[[nodiscard]] constexpr bool allows(PathPolicy policy, Fidelity fidelity) noexcept
{
    switch (policy) {
    case PathPolicy::exact_only:
        return fidelity == Fidelity::exact;
    case PathPolicy::automatic:
        return fidelity == Fidelity::exact || fidelity == Fidelity::repacked;
    case PathPolicy::processed:
        return false; // there is no processed graph to reach
    }
    return false;
}

/// What the sink actually gave us, against what we asked for.
[[nodiscard]] Fidelity classify(const Format& source, const Format& accepted) noexcept;

} // namespace mp

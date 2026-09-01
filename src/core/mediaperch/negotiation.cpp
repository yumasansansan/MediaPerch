// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/negotiation.hpp"

#include <array>

namespace mp {
namespace {

/// Integer PCM only: float is a Path B bus format, and moving into it is a
/// conversion however lossless it looks.
constexpr bool is_integer_pcm(SampleType t) noexcept
{
    return t == SampleType::s16 || t == SampleType::s24_packed ||
           t == SampleType::s24_in_32 || t == SampleType::s32;
}

void emit(std::vector<Candidate>& out, const Format& base, Fidelity fidelity)
{
    out.push_back(Candidate{base, fidelity, false});

    // The extensible form, paired with its own container rather than appended
    // after every other one -- see the header for why the order matters.
    if (base.channel_mask == 0) {
        const std::uint32_t mask = conventional_channel_mask(base.channels);
        if (mask != 0) {
            Format tagged = base;
            tagged.channel_mask = mask;
            out.push_back(Candidate{tagged, fidelity, true});
        }
    }
}

} // namespace

const char* path_policy_name(PathPolicy p) noexcept
{
    switch (p) {
    case PathPolicy::automatic:
        return "auto";
    case PathPolicy::exact_only:
        return "exact";
    case PathPolicy::processed:
        return "processed";
    }
    return "auto";
}

bool path_policy_from_name(std::string_view name, PathPolicy& out) noexcept
{
    if (name == "auto") {
        out = PathPolicy::automatic;
    } else if (name == "exact") {
        out = PathPolicy::exact_only;
    } else if (name == "processed") {
        out = PathPolicy::processed;
    } else {
        return false;
    }
    return true;
}

std::vector<Candidate> build_candidates(const Format& source, PathPolicy policy)
{
    std::vector<Candidate> out;
    if (!is_valid(source) || policy == PathPolicy::processed) {
        return out;
    }

    emit(out, source, Fidelity::exact);

    // Asked for a memcpy and nothing else: the source's own container, in both
    // its plain and its extensible form, and no further. Every candidate past
    // this point is a repack, and a repack is what was refused.
    if (policy == PathPolicy::exact_only) {
        return out;
    }

    // A DoP frame carries its markers in the top byte and a bitstream is not
    // samples at all. Neither survives being moved between containers. Float is
    // not repacked either: it has no left-justified integer representation.
    if (source.encoding != Encoding::pcm || !is_integer_pcm(source.sample_type)) {
        return out;
    }

    const std::uint32_t valid = effective_valid_bits(source);
    const std::uint32_t own = container_bytes(source.sample_type);

    // Small to large, so a device that takes several gets the cheapest wire
    // format rather than the widest.
    for (const std::uint32_t bytes : std::array<std::uint32_t, 3>{2, 3, 4}) {
        if (bytes == own) {
            continue; // already offered, exactly, above
        }
        const SampleType type = canonical_for(bytes, valid);
        if (type == SampleType::none) {
            continue; // this container cannot hold the signal
        }

        Format f = source;
        f.sample_type = type;
        f.valid_bits = valid; // the bits that carry signal do not change
        emit(out, f, Fidelity::repacked);
    }
    return out;
}

Fidelity classify(const Format& source, const Format& accepted) noexcept
{
    if (!is_valid(source) || !is_valid(accepted)) {
        return Fidelity::converted;
    }
    if (source.sample_rate != accepted.sample_rate || source.channels != accepted.channels ||
        source.encoding != accepted.encoding) {
        return Fidelity::converted;
    }
    // Naming a layout the source left unspecified changes no bytes. Naming a
    // different one reorders channels, which is a conversion.
    if (source.channel_mask != 0 && source.channel_mask != accepted.channel_mask) {
        return Fidelity::converted;
    }

    // Losing valid bits is the one thing a repack must never do.
    if (effective_valid_bits(accepted) != effective_valid_bits(source)) {
        return Fidelity::converted;
    }

    // Compare containers, not enum values: a four-byte container holding 24
    // valid bits is the same wire format whether it is spelled `s24_in_32` or
    // `s32` with `valid_bits = 24`.
    if (container_bytes(source.sample_type) == container_bytes(accepted.sample_type)) {
        return source.sample_type == SampleType::f32 ||
                       accepted.sample_type == SampleType::f32
                   ? (source.sample_type == accepted.sample_type ? Fidelity::exact
                                                                 : Fidelity::converted)
                   : Fidelity::exact;
    }

    if (is_integer_pcm(source.sample_type) && is_integer_pcm(accepted.sample_type) &&
        source.encoding == Encoding::pcm) {
        return Fidelity::repacked;
    }

    return Fidelity::converted;
}

} // namespace mp

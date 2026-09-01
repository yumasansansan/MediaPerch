// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/negotiation.hpp"

#include <algorithm>

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
    case PathPolicy::bit_exact:
        return "bitexact";
    case PathPolicy::exact_only:
        return "exact";
    case PathPolicy::automatic:
        return "auto";
    case PathPolicy::processed:
        return "processed";
    }
    return "bitexact";
}

bool path_policy_from_name(std::string_view name, PathPolicy& out) noexcept
{
    if (name == "bitexact") {
        out = PathPolicy::bit_exact;
    } else if (name == "exact") {
        out = PathPolicy::exact_only;
    } else if (name == "auto") {
        out = PathPolicy::automatic;
    } else if (name == "processed") {
        out = PathPolicy::processed;
    } else {
        return false;
    }
    return true;
}

/// The containers Path B may convert into, widest first.
///
/// Widest first because the conversion quantises, and every bit the destination
/// has is a bit the quantiser does not have to throw away. Float is not here:
/// if the device took float, the source's own format already did, and if it did
/// not then converting *to* float would not help.
void emit_conversions(std::vector<Candidate>& out, const Format& source)
{
    for (const SampleType type : {SampleType::s32, SampleType::s24_in_32,
                                  SampleType::s24_packed, SampleType::s16}) {
        Format f = source;
        f.sample_type = type;
        f.valid_bits = 0; // the destination's own width; nothing is being preserved
        const bool already = std::ranges::any_of(out, [&](const Candidate& c) {
            return c.format.sample_type == type;
        });
        if (!already) {
            emit(out, f, Fidelity::converted);
        }
    }
}

std::vector<Candidate> build_candidates(const Format& source, PathPolicy policy)
{
    std::vector<Candidate> out;
    if (!is_valid(source)) {
        return out;
    }

    // Path B was asked for by name. The candidate list is still every container
    // the device might take, the source's own included: a gain does not need a
    // different wire format, and forcing one would quantise for no reason.
    if (policy == PathPolicy::processed) {
        if (source.encoding == Encoding::pcm) {
            emit(out, source, Fidelity::exact);
            emit_conversions(out, source);
        }
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
    // samples at all. Neither survives being moved between containers, and
    // neither survives being converted either -- so for those two the exact
    // candidate is the whole list, whatever the policy says.
    if (source.encoding != Encoding::pcm) {
        return out;
    }

    // Float is not *repacked*: it has no left-justified integer representation.
    // It is very much converted, though, and skipping straight past this block
    // to the conversions is the entire reason Path B exists -- every lossy
    // decoder here reports F32.
    if (is_integer_pcm(source.sample_type)) {
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
    }

    // Last, and only when asked: everything above is bit-exact and is tried
    // first, so a device that can take the signal unaltered always does.
    if (policy == PathPolicy::automatic) {
        emit_conversions(out, source);
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/negotiation.hpp"

namespace mp {
namespace {

/// Integer PCM only: float is a Path B bus format, and promoting into it is a
/// conversion however lossless it looks.
constexpr bool is_integer_pcm(SampleType t) noexcept
{
    return t == SampleType::s16 || t == SampleType::s24_packed ||
           t == SampleType::s24_in_32 || t == SampleType::s32;
}

/// The containers a source type may be promoted into, widest last, each of them
/// holding the original bits unchanged.
void append_widenings(SampleType from, std::vector<SampleType>& out)
{
    switch (from) {
    case SampleType::s16:
        out.push_back(SampleType::s24_in_32);
        out.push_back(SampleType::s32);
        break;
    case SampleType::s24_packed:
        out.push_back(SampleType::s24_in_32);
        out.push_back(SampleType::s32);
        break;
    case SampleType::s24_in_32:
        out.push_back(SampleType::s32);
        break;
    case SampleType::s32:
    case SampleType::f32:
    case SampleType::none:
        break;
    }
}

void emit(std::vector<Candidate>& out, const Format& base, Fidelity fidelity)
{
    out.push_back(Candidate{base, fidelity, false});

    // The extensible form, paired with its own base rather than appended after
    // every widening -- see the header for why the order matters.
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

std::vector<Candidate> build_candidates(const Format& source)
{
    std::vector<Candidate> out;
    if (!is_valid(source)) {
        return out;
    }

    emit(out, source, Fidelity::exact);

    // A DoP frame carries its markers in the top byte and a bitstream is not
    // samples at all. Neither survives being moved into a wider container.
    if (source.encoding != Encoding::pcm) {
        return out;
    }

    std::vector<SampleType> wider;
    append_widenings(source.sample_type, wider);

    const std::uint32_t valid = effective_valid_bits(source);
    for (const SampleType type : wider) {
        Format f = source;
        f.sample_type = type;
        f.valid_bits = valid; // the bits that carry signal do not change
        emit(out, f, Fidelity::widened);
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

    if (source.sample_type == accepted.sample_type &&
        effective_valid_bits(source) == effective_valid_bits(accepted)) {
        return Fidelity::exact;
    }

    if (source.encoding == Encoding::pcm && is_integer_pcm(source.sample_type) &&
        is_integer_pcm(accepted.sample_type) &&
        container_bytes(accepted.sample_type) > container_bytes(source.sample_type) &&
        effective_valid_bits(accepted) >= effective_valid_bits(source)) {
        return Fidelity::widened;
    }

    return Fidelity::converted;
}

} // namespace mp

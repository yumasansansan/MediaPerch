// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/format.hpp"

#include <array>

namespace mp {
namespace {

constexpr const char* sample_type_name(SampleType t) noexcept
{
    switch (t) {
    case SampleType::none: return "NONE";
    case SampleType::s16: return "S16";
    case SampleType::s24_packed: return "S24_PACKED";
    case SampleType::s24_in_32: return "S24_IN_32";
    case SampleType::s32: return "S32";
    case SampleType::f32: return "F32";
    case SampleType::u8: return "U8";
    case SampleType::f64: return "F64";
    }
    return "?";
}

constexpr const char* encoding_name(Encoding e) noexcept
{
    switch (e) {
    case Encoding::pcm: return "PCM";
    case Encoding::dop: return "DoP";
    case Encoding::iec61937: return "IEC61937";
    }
    return "?";
}

} // namespace

std::uint32_t container_bytes(SampleType type) noexcept
{
    switch (type) {
    case SampleType::none: return 0;
    case SampleType::s16: return 2;
    case SampleType::s24_packed: return 3;
    case SampleType::s24_in_32: return 4;
    case SampleType::s32: return 4;
    case SampleType::f32: return 4;
    case SampleType::u8: return 1;
    case SampleType::f64: return 8;
    }
    return 0;
}

std::uint32_t natural_valid_bits(SampleType type) noexcept
{
    switch (type) {
    case SampleType::none: return 0;
    case SampleType::s16: return 16;
    case SampleType::s24_packed: return 24;
    case SampleType::s24_in_32: return 24;
    case SampleType::s32: return 32;
    case SampleType::f32: return 32;
    case SampleType::u8: return 8;
    case SampleType::f64: return 64;
    }
    return 0;
}

std::uint32_t effective_valid_bits(const Format& f) noexcept
{
    return f.valid_bits != 0 ? f.valid_bits : natural_valid_bits(f.sample_type);
}

std::uint32_t frame_bytes(const Format& f) noexcept
{
    return container_bytes(f.sample_type) * f.channels;
}

std::uint64_t bytes_per_second(const Format& f) noexcept
{
    return std::uint64_t{frame_bytes(f)} * f.sample_rate;
}

bool is_valid(const Format& f) noexcept
{
    if (f.sample_rate == 0 || f.channels == 0) {
        return false;
    }
    const std::uint32_t bytes = container_bytes(f.sample_type);
    if (bytes == 0) {
        return false;
    }
    if (f.valid_bits != 0 && f.valid_bits > bytes * 8) {
        return false;
    }
    // A mask that names fewer positions than there are channels would leave the
    // sink to guess, which is exactly what the extensible form exists to avoid.
    if (f.channel_mask != 0) {
        std::uint32_t named = 0;
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            named += (f.channel_mask >> bit) & 1u;
        }
        if (named != f.channels) {
            return false;
        }
    }
    // Float is a Path B bus format. It never reaches an exclusive endpoint as
    // DoP or as a bitstream.
    if (f.encoding != Encoding::pcm && f.sample_type == SampleType::f32) {
        return false;
    }
    return true;
}

SampleType canonical_for(std::uint32_t container_bytes, std::uint32_t valid_bits) noexcept
{
    if (valid_bits == 0 || valid_bits > container_bytes * 8) {
        return SampleType::none;
    }
    switch (container_bytes) {
    case 2: return SampleType::s16;
    case 3: return SampleType::s24_packed;
    case 4: return valid_bits <= 24 ? SampleType::s24_in_32 : SampleType::s32;
    default: return SampleType::none;
    }
}

std::uint32_t conventional_channel_mask(std::uint32_t channels) noexcept
{
    switch (channels) {
    case 1:
        return MP_SPEAKER_FRONT_CENTER;
    case 2:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT;
    case 4:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_BACK_LEFT |
               MP_SPEAKER_BACK_RIGHT;
    case 6:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_FRONT_CENTER |
               MP_SPEAKER_LOW_FREQUENCY | MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;
    case 8:
        return MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT | MP_SPEAKER_FRONT_CENTER |
               MP_SPEAKER_LOW_FREQUENCY | MP_SPEAKER_BACK_LEFT | MP_SPEAKER_BACK_RIGHT |
               MP_SPEAKER_SIDE_LEFT | MP_SPEAKER_SIDE_RIGHT;
    default:
        // 3, 5, 7 and anything above 8 have no single conventional layout, and
        // guessing one is worse than leaving the sink its default.
        return 0;
    }
}

std::string describe(const Format& f)
{
    std::string out;
    out.reserve(64);
    out += std::to_string(f.sample_rate);
    out += " Hz / ";
    out += std::to_string(f.channels);
    out += " ch / ";
    out += sample_type_name(f.sample_type);

    const std::uint32_t valid = effective_valid_bits(f);
    if (valid != natural_valid_bits(f.sample_type)) {
        out += " (";
        out += std::to_string(valid);
        out += " valid)";
    }
    if (f.encoding != Encoding::pcm) {
        out += " / ";
        out += encoding_name(f.encoding);
    }
    if (f.channel_mask != 0) {
        out += " / mask 0x";
        constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        bool leading = true;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const auto nibble = static_cast<std::size_t>((f.channel_mask >> shift) & 0xFu);
            if (nibble == 0 && leading && shift != 0) {
                continue;
            }
            leading = false;
            out += digits[nibble];
        }
    }
    return out;
}

MpFormat to_abi(const Format& f) noexcept
{
    MpFormat out{};
    out.sample_rate = f.sample_rate;
    out.channels = f.channels;
    out.channel_mask = f.channel_mask;
    out.sample_type = static_cast<MpSampleType>(f.sample_type);
    out.encoding = static_cast<MpEncoding>(f.encoding);
    out.valid_bits = f.valid_bits;
    return out;
}

Format from_abi(const MpFormat& f) noexcept
{
    Format out;
    out.sample_rate = f.sample_rate;
    out.channels = f.channels;
    out.channel_mask = f.channel_mask;
    out.sample_type = static_cast<SampleType>(f.sample_type);
    out.encoding = static_cast<Encoding>(f.encoding);
    out.valid_bits = f.valid_bits;
    return out;
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcm_format.hpp"

namespace mp::pcm {

std::uint32_t container_for(std::uint32_t bits) noexcept
{
    if (bits == 0 || bits > 32) {
        return 0;
    }
    if (bits <= 16) {
        return 2;
    }
    if (bits <= 24) {
        return 3;
    }
    return 4;
}

MpSampleType sample_type_for(std::uint32_t container, std::uint32_t valid) noexcept
{
    if (valid == 0 || valid > container * 8) {
        return MP_SAMPLE_NONE;
    }
    switch (container) {
    case 2:
        return MP_SAMPLE_S16;
    case 3:
        return MP_SAMPLE_S24_PACKED;
    case 4:
        // **24 in 32 is not 32.** A four-byte slot holding 24 significant bits
        // is what a device is told, so that it can offer the format the file
        // actually has rather than one eight bits wider.
        return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    default:
        return MP_SAMPLE_NONE;
    }
}

MpSampleType float_sample_type(std::uint32_t bits) noexcept
{
    switch (bits) {
    case 32:
        return MP_SAMPLE_F32;
    case 64:
        return MP_SAMPLE_F64;
    default:
        // Nothing else is a float IEEE defines, and half is not audio.
        return MP_SAMPLE_NONE;
    }
}

} // namespace mp::pcm

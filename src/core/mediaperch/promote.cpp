// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/promote.hpp"

#include <cstdint>
#include <cstring>

namespace mp {
namespace {

/// Reads a 24-bit little-endian sample and returns it left-justified in 32 bits.
std::int32_t read_s24_packed(const std::uint8_t* p) noexcept
{
    const auto raw = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                     (static_cast<std::uint32_t>(p[2]) << 16);
    // Shift up by 8 rather than sign-extending down: the destination wants the
    // valid bits at the top, so the sign bit lands where it belongs by itself.
    return static_cast<std::int32_t>(raw << 8);
}

void write_le32(std::uint8_t* p, std::int32_t v) noexcept
{
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(u & 0xFFu);
    p[1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((u >> 24) & 0xFFu);
}

std::int16_t read_le16(const std::uint8_t* p) noexcept
{
    const auto raw = static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                                (static_cast<std::uint16_t>(p[1]) << 8));
    return static_cast<std::int16_t>(raw);
}

} // namespace

std::size_t promoted_bytes(SampleType to, std::size_t samples) noexcept
{
    return static_cast<std::size_t>(container_bytes(to)) * samples;
}

bool promote(const void* src, SampleType from, void* dst, SampleType to,
             std::size_t samples) noexcept
{
    const auto* in = static_cast<const std::uint8_t*>(src);
    auto* out = static_cast<std::uint8_t*>(dst);

    // s24_in_32 into s32 is the degenerate case: the bits are already at the top
    // of a four-byte container, so widening the *declared* width moves nothing.
    if (from == SampleType::s24_in_32 && to == SampleType::s32) {
        std::memcpy(out, in, samples * 4);
        return true;
    }

    if (from == SampleType::s16 && (to == SampleType::s24_in_32 || to == SampleType::s32)) {
        for (std::size_t i = 0; i < samples; ++i) {
            const std::int32_t v = static_cast<std::int32_t>(read_le16(in + i * 2)) << 16;
            write_le32(out + i * 4, v);
        }
        return true;
    }

    if (from == SampleType::s24_packed &&
        (to == SampleType::s24_in_32 || to == SampleType::s32)) {
        for (std::size_t i = 0; i < samples; ++i) {
            write_le32(out + i * 4, read_s24_packed(in + i * 3));
        }
        return true;
    }

    return false;
}

} // namespace mp

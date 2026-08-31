// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/repack.hpp"

#include <cstdint>
#include <cstring>

namespace mp {
namespace {

constexpr bool is_integer_pcm(SampleType t) noexcept
{
    return t == SampleType::s16 || t == SampleType::s24_packed ||
           t == SampleType::s24_in_32 || t == SampleType::s32;
}

} // namespace

std::size_t repacked_bytes(SampleType to, std::size_t samples) noexcept
{
    return static_cast<std::size_t>(container_bytes(to)) * samples;
}

bool repack(const void* src, SampleType from, void* dst, SampleType to,
            std::uint32_t valid_bits, std::size_t samples) noexcept
{
    if (!is_integer_pcm(from) || !is_integer_pcm(to)) {
        return false;
    }

    const std::uint32_t from_bytes = container_bytes(from);
    const std::uint32_t to_bytes = container_bytes(to);
    if (from_bytes == 0 || to_bytes == 0) {
        return false;
    }

    // Dropping bytes off the bottom is lossless only while they are padding.
    if (valid_bits == 0 || valid_bits > to_bytes * 8 || valid_bits > from_bytes * 8) {
        return false;
    }

    const auto* in = static_cast<const std::uint8_t*>(src);
    auto* out = static_cast<std::uint8_t*>(dst);

    if (from_bytes == to_bytes) {
        // Same container: the declared valid-bit count may differ, but the bytes
        // on the wire do not.
        std::memcpy(out, in, samples * to_bytes);
        return true;
    }

    // Little-endian, left-justified: the most significant byte is last, so the
    // shared part of the two containers is their tail, and the padding is at the
    // head.
    const std::uint32_t kept = from_bytes < to_bytes ? from_bytes : to_bytes;
    const std::uint32_t pad = to_bytes - kept; // zero when shrinking
    const std::uint32_t skip = from_bytes - kept; // zero when growing

    for (std::size_t i = 0; i < samples; ++i) {
        std::uint8_t* o = out + i * to_bytes;
        const std::uint8_t* s = in + i * from_bytes;
        for (std::uint32_t b = 0; b < pad; ++b) {
            o[b] = 0;
        }
        std::memcpy(o + pad, s + skip, kept);
    }
    return true;
}

} // namespace mp

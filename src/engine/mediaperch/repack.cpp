// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/repack.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mp {
namespace {

constexpr bool is_integer_pcm(SampleType t) noexcept
{
    return t == SampleType::s16 || t == SampleType::s24_packed ||
           t == SampleType::s24_in_32 || t == SampleType::s32;
}

/// One pair of containers, with both sizes known to the compiler: each sample
/// read as the little-endian number its bytes hold, moved to the top of the
/// other container, and written back a byte at a time.
///
/// **As a number, every pair is a loop the compiler makes vectors of.** The
/// bytes copied with sizes known only at run time were a call to `memcpy` for
/// every sample; copied with the sizes known, the two zero bytes under a 16-bit
/// sample in four became one 16-bit store beside two byte stores, and the
/// vectoriser would not take that loop.
template <std::size_t From, std::size_t To>
void repack_as(const std::uint8_t* in, std::uint8_t* out, std::size_t samples) noexcept
{
    for (std::size_t i = 0; i < samples; ++i) {
        const std::uint8_t* s = in + i * From;
        std::uint32_t v = 0;
        for (std::size_t b = 0; b < From; ++b) {
            v |= std::uint32_t{s[b]} << (8 * b);
        }
        if constexpr (To > From) {
            v <<= 8 * (To - From);
        } else {
            v >>= 8 * (From - To);
        }
        std::uint8_t* o = out + i * To;
        for (std::size_t b = 0; b < To; ++b) {
            o[b] = static_cast<std::uint8_t>(v >> (8 * b));
        }
    }
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
    if (from_bytes == 2 && to_bytes == 3) {
        repack_as<2, 3>(in, out, samples);
    } else if (from_bytes == 2 && to_bytes == 4) {
        repack_as<2, 4>(in, out, samples);
    } else if (from_bytes == 3 && to_bytes == 2) {
        repack_as<3, 2>(in, out, samples);
    } else if (from_bytes == 3 && to_bytes == 4) {
        repack_as<3, 4>(in, out, samples);
    } else if (from_bytes == 4 && to_bytes == 2) {
        repack_as<4, 2>(in, out, samples);
    } else if (from_bytes == 4 && to_bytes == 3) {
        repack_as<4, 3>(in, out, samples);
    } else {
        return false;
    }
    return true;
}

} // namespace mp

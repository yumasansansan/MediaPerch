// SPDX-License-Identifier: GPL-3.0-or-later
//
// A rate as the two integers it is.
//
// **60000/1001 is not a double**, and neither is 24000/1001, and the whole of
// what this tree does with frame rates and refresh rates turns on that: whether
// one divides the other is a remainder, not a tolerance. `refresh.hpp` decides
// which display mode suits a film with it; `framerate.hpp` reads a container's
// rounded rate back with it. It is here rather than in either because it
// belongs to both.
//
// The ABI states rates the same way -- `MpVideoInfo::fps_num` and `fps_den` --
// so this is that pair with a name.

#ifndef MEDIAPERCH_RATIONAL_HPP
#define MEDIAPERCH_RATIONAL_HPP

#include <cstdint>

namespace mp {

struct Rational {
    std::uint32_t num = 0;
    std::uint32_t den = 0;

    [[nodiscard]] bool valid() const noexcept { return num != 0 && den != 0; }
    [[nodiscard]] double hz() const noexcept
    {
        return den != 0 ? static_cast<double>(num) / den : 0.0;
    }
    friend bool operator==(const Rational& a, const Rational& b) noexcept
    {
        // Cross-multiplied, so 60/1 and 120/2 are the same rate. In 64 bits,
        // because the product of two 32-bit numerators is not a 32-bit number.
        return static_cast<std::uint64_t>(a.num) * b.den ==
               static_cast<std::uint64_t>(b.num) * a.den;
    }
};

} // namespace mp

#endif // MEDIAPERCH_RATIONAL_HPP

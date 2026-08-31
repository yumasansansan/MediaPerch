// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/promote.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

std::int32_t read_le32(const std::uint8_t* p)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                     (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) |
                                     (static_cast<std::uint32_t>(p[3]) << 24));
}

} // namespace

TEST_CASE("16-bit promotes into 32 bits left-justified", "[promote]")
{
    // The convention WAVEFORMATEXTENSIBLE states: valid bits at the top, zeros
    // underneath. Getting it backwards does not fail -- it plays 48 dB quiet,
    // which is why this is spelled out rather than assumed.
    const std::array<std::int16_t, 4> in{0, 1, -1, 32767};
    std::array<std::uint8_t, 16> out{};

    REQUIRE(mp::promote(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::s24_in_32,
                        in.size()));

    CHECK(read_le32(out.data() + 0) == 0);
    CHECK(read_le32(out.data() + 4) == (1 << 16));
    CHECK(read_le32(out.data() + 8) == (-1 << 16));
    CHECK(read_le32(out.data() + 12) == (32767 << 16));
}

TEST_CASE("promotion preserves sign and never rescales", "[promote]")
{
    const std::array<std::int16_t, 2> in{-32768, 12345};
    std::array<std::uint8_t, 8> out{};

    REQUIRE(mp::promote(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::s32,
                        in.size()));

    // A left shift, not a multiply-and-round: the low 16 bits are exactly zero
    // and the top 16 are the original sample.
    CHECK(read_le32(out.data() + 0) == (static_cast<std::int32_t>(-32768) << 16));
    CHECK(read_le32(out.data() + 4) == (12345 << 16));
    CHECK((read_le32(out.data() + 4) & 0xFFFF) == 0);
}

TEST_CASE("packed 24-bit promotes into 32 bits", "[promote]")
{
    // 0x7FFFFF, 0x800000 (the most negative), 0x000001
    const std::array<std::uint8_t, 9> in{0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00};
    std::array<std::uint8_t, 12> out{};

    REQUIRE(mp::promote(in.data(), mp::SampleType::s24_packed, out.data(),
                        mp::SampleType::s24_in_32, 3));

    CHECK(read_le32(out.data() + 0) == static_cast<std::int32_t>(0x7FFFFF00));
    CHECK(read_le32(out.data() + 4) == static_cast<std::int32_t>(0x80000000));
    CHECK(read_le32(out.data() + 8) == 0x00000100);
}

TEST_CASE("24-in-32 into 32 is a copy, because the bits are already at the top",
          "[promote]")
{
    const std::array<std::uint8_t, 8> in{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::array<std::uint8_t, 8> out{};

    REQUIRE(mp::promote(in.data(), mp::SampleType::s24_in_32, out.data(), mp::SampleType::s32,
                        2));
    CHECK(out == in);
}

TEST_CASE("promote refuses anything that is not a widening", "[promote]")
{
    std::array<std::uint8_t, 32> out{};
    const std::array<std::uint8_t, 32> in{};

    // Narrowing.
    CHECK_FALSE(mp::promote(in.data(), mp::SampleType::s32, out.data(), mp::SampleType::s16, 4));
    // Same width.
    CHECK_FALSE(mp::promote(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::s16, 4));
    // Into float: lossless-looking, still a conversion, still Path B.
    CHECK_FALSE(mp::promote(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::f32, 4));
    // Out of float.
    CHECK_FALSE(mp::promote(in.data(), mp::SampleType::f32, out.data(), mp::SampleType::s32, 4));
}

TEST_CASE("promoted_bytes matches what promote writes", "[promote]")
{
    CHECK(mp::promoted_bytes(mp::SampleType::s24_in_32, 10) == 40);
    CHECK(mp::promoted_bytes(mp::SampleType::s32, 10) == 40);
    CHECK(mp::promoted_bytes(mp::SampleType::s16, 10) == 20);
}

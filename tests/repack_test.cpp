// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/repack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::int32_t read_le32(const std::uint8_t* p)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                     (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) |
                                     (static_cast<std::uint32_t>(p[3]) << 24));
}

} // namespace

TEST_CASE("16-bit into a four-byte container is left-justified", "[repack]")
{
    // The convention WAVEFORMATEXTENSIBLE states: valid bits at the top, zeros
    // underneath. Getting it backwards does not fail -- it plays 48 dB quiet,
    // which is why this is spelled out rather than assumed.
    const std::array<std::int16_t, 4> in{0, 1, -1, 32767};
    std::array<std::uint8_t, 16> out{};

    REQUIRE(mp::repack(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::s24_in_32,
                       16, in.size()));

    CHECK(read_le32(out.data() + 0) == 0);
    CHECK(read_le32(out.data() + 4) == (1 << 16));
    CHECK(read_le32(out.data() + 8) == (-1 << 16));
    CHECK(read_le32(out.data() + 12) == (32767 << 16));
}

TEST_CASE("16-bit into a three-byte container is also left-justified", "[repack]")
{
    // The container a virtual cable configured for "24 bit" actually wanted.
    const std::array<std::int16_t, 2> in{0x1234, -1};
    std::array<std::uint8_t, 6> out{};

    REQUIRE(mp::repack(in.data(), mp::SampleType::s16, out.data(),
                       mp::SampleType::s24_packed, 16, in.size()));

    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x34);
    CHECK(out[2] == 0x12);
    CHECK(out[3] == 0x00);
    CHECK(out[4] == 0xFF);
    CHECK(out[5] == 0xFF);
}

TEST_CASE("packed 24-bit moves into four bytes", "[repack]")
{
    // 0x7FFFFF, 0x800000 (the most negative), 0x000001
    const std::array<std::uint8_t, 9> in{0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00};
    std::array<std::uint8_t, 12> out{};

    REQUIRE(mp::repack(in.data(), mp::SampleType::s24_packed, out.data(),
                       mp::SampleType::s24_in_32, 24, 3));

    CHECK(read_le32(out.data() + 0) == static_cast<std::int32_t>(0x7FFFFF00));
    CHECK(read_le32(out.data() + 4) == static_cast<std::int32_t>(0x80000000));
    CHECK(read_le32(out.data() + 8) == 0x00000100);
}

TEST_CASE("four-byte 24-bit moves back into three bytes", "[repack]")
{
    // The direction the original `promote` could not do, and the one a real
    // device asked for. Nothing is lost: the byte dropped off the bottom is the
    // padding, and the candidate list only offers this pair when it is.
    const std::array<std::uint8_t, 8> in{0x00, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80};
    std::array<std::uint8_t, 6> out{};

    REQUIRE(mp::repack(in.data(), mp::SampleType::s24_in_32, out.data(),
                       mp::SampleType::s24_packed, 24, 2));

    CHECK(out[0] == 0xFF);
    CHECK(out[1] == 0xFF);
    CHECK(out[2] == 0x7F);
    CHECK(out[3] == 0x00);
    CHECK(out[4] == 0x00);
    CHECK(out[5] == 0x80);
}

TEST_CASE("a repack round trip is the identity", "[repack]")
{
    // Left-justification is what makes this true in both directions, and a
    // round trip is the cheapest way to keep the two halves honest about it.
    std::array<std::uint8_t, 64> original{};
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }

    std::array<std::uint8_t, 96> wide{};
    std::array<std::uint8_t, 64> back{};

    REQUIRE(mp::repack(original.data(), mp::SampleType::s16, wide.data(),
                       mp::SampleType::s24_packed, 16, 32));
    REQUIRE(mp::repack(wide.data(), mp::SampleType::s24_packed, back.data(),
                       mp::SampleType::s16, 16, 32));
    CHECK(back == original);
}

TEST_CASE("every pair of containers keeps the top bytes, at every length", "[repack]")
{
    // The rule byte by byte -- the top `min(from, to)` bytes of each sample,
    // zeros below them -- against every pair of different containers, at
    // lengths either side of what a vector holds. Each pair is a loop the
    // compiler makes vectors of, with the samples a vector does not fill done
    // one at a time, and a mistake in either half shows at some lengths and
    // not at others.
    const mp::SampleType types[] = {mp::SampleType::s16, mp::SampleType::s24_packed,
                                    mp::SampleType::s24_in_32, mp::SampleType::s32};
    const std::size_t lengths[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                                   63, 64, 65, 127, 128, 129, 1000};
    for (const mp::SampleType from : types) {
        for (const mp::SampleType to : types) {
            const std::uint32_t from_bytes = mp::container_bytes(from);
            const std::uint32_t to_bytes = mp::container_bytes(to);
            if (from_bytes == to_bytes) {
                continue;
            }
            const std::uint32_t kept = std::min(from_bytes, to_bytes);
            const std::uint32_t pad = to_bytes - kept;
            const std::uint32_t skip = from_bytes - kept;
            for (const std::size_t samples : lengths) {
                std::vector<std::uint8_t> in(samples * from_bytes);
                for (std::size_t i = 0; i < in.size(); ++i) {
                    in[i] = static_cast<std::uint8_t>((i * 131u + 7u) & 0xFFu);
                }
                std::vector<std::uint8_t> out(samples * to_bytes, 0xA5);
                REQUIRE(mp::repack(in.data(), from, out.data(), to, 8 * kept, samples));

                std::size_t wrong = 0;
                for (std::size_t i = 0; i < samples; ++i) {
                    for (std::uint32_t b = 0; b < to_bytes; ++b) {
                        const std::uint8_t want =
                            b < pad ? std::uint8_t{0} : in[i * from_bytes + skip + (b - pad)];
                        if (out[i * to_bytes + b] != want) {
                            ++wrong;
                        }
                    }
                }
                INFO(from_bytes << " bytes into " << to_bytes << ", " << samples << " samples");
                CHECK(wrong == 0);
            }
        }
    }
}

TEST_CASE("same container is a copy however the type is spelled", "[repack]")
{
    // s24_in_32 and s32 are the same four bytes on the wire; only the declared
    // valid-bit count differs, and that is metadata, not signal.
    const std::array<std::uint8_t, 8> in{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::array<std::uint8_t, 8> out{};

    REQUIRE(mp::repack(in.data(), mp::SampleType::s24_in_32, out.data(), mp::SampleType::s32,
                       24, 2));
    CHECK(out == in);
}

TEST_CASE("repack refuses to drop signal", "[repack]")
{
    std::array<std::uint8_t, 32> out{};
    const std::array<std::uint8_t, 32> in{};

    // 32 valid bits do not fit in two bytes, and pretending they do is the one
    // way this function could lose something.
    CHECK_FALSE(
        mp::repack(in.data(), mp::SampleType::s32, out.data(), mp::SampleType::s16, 32, 4));
    CHECK_FALSE(mp::repack(in.data(), mp::SampleType::s24_in_32, out.data(),
                           mp::SampleType::s16, 24, 4));
    // Zero valid bits is not a format.
    CHECK_FALSE(
        mp::repack(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::s32, 0, 4));
}

TEST_CASE("repack refuses float in either direction", "[repack]")
{
    std::array<std::uint8_t, 32> out{};
    const std::array<std::uint8_t, 32> in{};

    CHECK_FALSE(
        mp::repack(in.data(), mp::SampleType::s16, out.data(), mp::SampleType::f32, 16, 4));
    CHECK_FALSE(
        mp::repack(in.data(), mp::SampleType::f32, out.data(), mp::SampleType::s32, 32, 4));
}

TEST_CASE("repacked_bytes matches what repack writes", "[repack]")
{
    CHECK(mp::repacked_bytes(mp::SampleType::s16, 10) == 20);
    CHECK(mp::repacked_bytes(mp::SampleType::s24_packed, 10) == 30);
    CHECK(mp::repacked_bytes(mp::SampleType::s24_in_32, 10) == 40);
    CHECK(mp::repacked_bytes(mp::SampleType::s32, 10) == 40);
}

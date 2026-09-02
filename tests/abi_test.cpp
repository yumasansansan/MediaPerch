// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

extern "C" unsigned int mp_abi_probe_from_c(void);

TEST_CASE("the ABI header agrees with itself across C and C++", "[abi]")
{
    // abi_header_c.c is compiled as C. If its MP_STATIC_ASSERT block disagreed
    // with the C++ one the build would already have failed; this checks that the
    // two objects also link and see the same constant.
    REQUIRE(mp_abi_probe_from_c() == MP_ABI_VERSION);
}

TEST_CASE("MpFormat layout is fixed", "[abi]")
{
    STATIC_REQUIRE(sizeof(MpFormat) == 32);
    STATIC_REQUIRE(offsetof(MpFormat, sample_rate) == 0);
    STATIC_REQUIRE(offsetof(MpFormat, valid_bits) == 20);
    STATIC_REQUIRE(alignof(MpFormat) == 4);
}

TEST_CASE("every vtable and descriptor opens with a size field", "[abi]")
{
    // Rule 2 of the ABI: new fields append, and a host reading an older module
    // clamps at `size`. That only works if `size` is at offset zero everywhere.
    STATIC_REQUIRE(offsetof(MpHost, size) == 0);
    STATIC_REQUIRE(offsetof(MpDemuxVtbl, size) == 0);
    STATIC_REQUIRE(offsetof(MpCodecVtbl, size) == 0);
    STATIC_REQUIRE(offsetof(MpSinkVtbl, size) == 0);
    STATIC_REQUIRE(offsetof(MpDspVtbl, size) == 0);
    STATIC_REQUIRE(offsetof(MpModuleDesc, size) == 0);
    STATIC_REQUIRE(offsetof(MpDeviceInfo, size) == 0);
}

TEST_CASE("the enum mirrors in core match the ABI constants", "[abi]")
{
    // format.hpp restates the ABI values as scoped enums so the core reads well.
    // A drift between the two would be silent and would corrupt every format
    // that crossed the boundary.
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::SampleType::s16) == MP_SAMPLE_S16);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::SampleType::s24_packed) ==
                   MP_SAMPLE_S24_PACKED);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::SampleType::s24_in_32) ==
                   MP_SAMPLE_S24_IN_32);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::SampleType::s32) == MP_SAMPLE_S32);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::SampleType::f32) == MP_SAMPLE_F32);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::Encoding::dop) == MP_ENCODING_DOP);
    STATIC_REQUIRE(static_cast<std::uint32_t>(mp::Encoding::iec61937) == MP_ENCODING_IEC61937);
}

TEST_CASE("a format survives a round trip through the ABI struct", "[abi]")
{
    const mp::Format source{
        .sample_rate = 96000,
        .channels = 6,
        .channel_mask = mp::conventional_channel_mask(6),
        .sample_type = mp::SampleType::s24_in_32,
        .encoding = mp::Encoding::pcm,
        .valid_bits = 24,
    };

    REQUIRE(mp::from_abi(mp::to_abi(source)) == source);
}

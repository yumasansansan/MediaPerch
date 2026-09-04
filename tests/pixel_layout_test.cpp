// SPDX-License-Identifier: GPL-3.0-or-later
//
// ABI v4's arithmetic, which is the whole of what v4 added.
//
// **v3 named six pixel formats; v4 describes them**, and the reason is that
// naming does not scale: five chroma layouts by five depths by three packings
// is seventy-five enumerators, and a decoder that produces 4:4:4 twelve-bit
// would have had nowhere to say so. What replaced them is six fields and some
// arithmetic over them -- so the arithmetic is what has to be right, and it is
// checkable with no GPU, no decoder and no file.
//
// The sharpest of it is `shift`. Ten bits in a sixteen-bit container is not
// one thing: P010 puts them at the top and dav1d puts them at the bottom, and
// a consumer that assumes either is wrong about the other by a factor of
// sixty-four -- as brightness rather than as an error. The tests below hold
// the general formula against the constant `yuv_matrix.cpp` carried before v4,
// which is how a refactor gets to be a refactor.

#include <mediaperch/module.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

constexpr MpPixelLayout k_nv12 = MP_LAYOUT_NV12;
constexpr MpPixelLayout k_p010 = MP_LAYOUT_P010;
constexpr MpPixelLayout k_bgra8 = MP_LAYOUT_BGRA8;
constexpr MpPixelLayout k_rgba16f = MP_LAYOUT_RGBA16F;
constexpr MpPixelLayout k_rgb10a2 = MP_LAYOUT_RGB10A2;
constexpr MpPixelLayout k_rgba32f = MP_LAYOUT_RGBA32F;

/// A planar layout built by hand, which is how a decoder that is not here yet
/// would state itself.
MpPixelLayout planar(MpChroma chroma, std::uint32_t bits, std::uint32_t container_bits)
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    out.chroma = chroma;
    out.packing = MP_PACK_PLANAR;
    out.bits = bits;
    out.container_bits = container_bits;
    return out;
}

} // namespace

TEST_CASE("the sample scale is derived, and it agrees with what was tabulated",
          "[video][abi][pixel]")
{
    // **The number this replaced.** yuv_matrix.cpp carried
    // `65535.0 / (1023.0 * 64.0)` for P010 and 1 for everything else, chosen
    // by a bool called `ten_bit`. If the general formula did not land on the
    // same value the refactor would be a rewrite, and this is the difference.
    CHECK(mp_pixel_sample_scale(&k_p010) ==
          Catch::Approx(65535.0 / (1023.0 * 64.0)).epsilon(0));
    CHECK(mp_pixel_sample_scale(&k_nv12) == Catch::Approx(1.0).epsilon(0));

    // And the case the bool could not tell apart from P010: the same ten bits
    // in the same sixteen, at the bottom instead of the top. Sixty-four times
    // out, and it is what dav1d will hand over.
    MpPixelLayout low = k_p010;
    low.shift = 0;
    CHECK(mp_pixel_sample_scale(&low) == Catch::Approx(65535.0 / 1023.0).epsilon(0));
    CHECK(mp_pixel_sample_scale(&low) ==
          Catch::Approx(mp_pixel_sample_scale(&k_p010) * 64.0).epsilon(1e-12));

    // Twelve, fourteen and sixteen bits, none of which v3 could say at all.
    // Sixteen in sixteen is exactly one, which is the check that the formula
    // is not off by a container.
    const MpPixelLayout i444_12 = planar(MP_CHROMA_444, 12u, 16u);
    const MpPixelLayout i422_14 = planar(MP_CHROMA_422, 14u, 16u);
    const MpPixelLayout i444_16 = planar(MP_CHROMA_444, 16u, 16u);
    const MpPixelLayout mono8 = planar(MP_CHROMA_MONO, 8u, 8u);
    CHECK(mp_pixel_sample_scale(&i444_12) == Catch::Approx(65535.0 / 4095.0).epsilon(0));
    CHECK(mp_pixel_sample_scale(&i422_14) == Catch::Approx(65535.0 / 16383.0).epsilon(0));
    CHECK(mp_pixel_sample_scale(&i444_16) == Catch::Approx(1.0).epsilon(0));
    CHECK(mp_pixel_sample_scale(&mono8) == Catch::Approx(1.0).epsilon(0));

    // Float carries its own value and is never scaled -- and the guard matters
    // because 1 << 32 is undefined and 32-bit floats are the read_back format.
    CHECK(mp_pixel_sample_scale(&k_rgba32f) == Catch::Approx(1.0).epsilon(0));
    CHECK(mp_pixel_sample_scale(&k_rgba16f) == Catch::Approx(1.0).epsilon(0));

    // A zeroed layout is a frame that says nothing, and asking it for a scale
    // must not divide by zero or shift by a hundred.
    MpPixelLayout nothing{};
    CHECK(mp_pixel_sample_scale(&nothing) == Catch::Approx(1.0).epsilon(0));
}

TEST_CASE("plane count and chroma size come out of the layout", "[video][abi][pixel]")
{
    CHECK(mp_pixel_planes(&k_nv12) == 2u);
    CHECK(mp_pixel_planes(&k_p010) == 2u);
    CHECK(mp_pixel_planes(&k_bgra8) == 1u);
    const MpPixelLayout i444 = planar(MP_CHROMA_444, 8u, 8u);
    const MpPixelLayout i420 = planar(MP_CHROMA_420, 8u, 8u);
    const MpPixelLayout mono = planar(MP_CHROMA_MONO, 8u, 8u);
    CHECK(mp_pixel_planes(&i444) == 3u);
    CHECK(mp_pixel_planes(&i420) == 3u);
    // 4:0:0 has one plane whatever the packing says, because there is no
    // chroma to put in a second.
    CHECK(mp_pixel_planes(&mono) == 1u);

    // **Rounded up, and an odd size is the case that says so.** 17 pixels of
    // 4:2:0 chroma is 9 columns, not 8: the last pixel still has a chroma
    // sample and rounding down drops the right-hand edge of the picture.
    CHECK(mp_pixel_chroma_width(&k_nv12, 16u) == 8u);
    CHECK(mp_pixel_chroma_width(&k_nv12, 17u) == 9u);
    CHECK(mp_pixel_chroma_height(&k_nv12, 17u) == 9u);

    const MpPixelLayout i422 = planar(MP_CHROMA_422, 8u, 8u);
    CHECK(mp_pixel_chroma_width(&i422, 17u) == 9u);
    CHECK(mp_pixel_chroma_height(&i422, 17u) == 17u); // 4:2:2 halves width only

    CHECK(mp_pixel_chroma_width(&i444, 17u) == 17u);
    CHECK(mp_pixel_chroma_height(&i444, 17u) == 17u);

    CHECK(mp_pixel_shift_x(&k_nv12) == 1u);
    CHECK(mp_pixel_shift_y(&k_nv12) == 1u);
    CHECK(mp_pixel_shift_x(&i422) == 1u);
    CHECK(mp_pixel_shift_y(&i422) == 0u);
}

TEST_CASE("how many bytes a pixel takes, including the packed one",
          "[video][abi][pixel]")
{
    CHECK(mp_pixel_components(&k_bgra8) == 4u);
    const MpPixelLayout i444 = planar(MP_CHROMA_444, 8u, 8u);
    const MpPixelLayout mono = planar(MP_CHROMA_MONO, 8u, 8u);
    CHECK(mp_pixel_components(&i444) == 3u);
    CHECK(mp_pixel_components(&mono) == 1u);

    CHECK(mp_pixel_bytes(&k_bgra8) == 4u);
    CHECK(mp_pixel_bytes(&k_rgba16f) == 8u);
    CHECK(mp_pixel_bytes(&k_rgba32f) == 16u);
    // **HDR10 is the one where components times container bytes is wrong.**
    // Four components in a single u32, so the flag exists and this is what it
    // is for -- 16 would be the answer without it, and read_back would ask for
    // four times the buffer it needs.
    CHECK((k_rgb10a2.flags & MP_PIXEL_PACKED) != 0u);
    CHECK(mp_pixel_bytes(&k_rgb10a2) == 4u);

    CHECK(mp_pixel_component_bytes(&k_nv12) == 1u);
    CHECK(mp_pixel_component_bytes(&k_p010) == 2u);
}

TEST_CASE("the well-known layouts say what their names always meant",
          "[video][abi][pixel]")
{
    // The macros are shorthand, not a second vocabulary, so what matters is
    // that each expands to exactly the fields its name has always meant.
    CHECK(k_nv12.chroma == MP_CHROMA_420);
    CHECK(k_nv12.packing == MP_PACK_SEMI_PLANAR);
    CHECK(k_nv12.bits == 8u);
    CHECK(k_nv12.container_bits == 8u);
    CHECK(k_nv12.shift == 0u);

    CHECK(k_p010.chroma == MP_CHROMA_420);
    CHECK(k_p010.packing == MP_PACK_SEMI_PLANAR);
    CHECK(k_p010.bits == 10u);
    CHECK(k_p010.container_bits == 16u);
    CHECK(k_p010.shift == 6u); // ten bits at the TOP of sixteen

    CHECK(k_bgra8.chroma == MP_CHROMA_RGB);
    CHECK((k_bgra8.flags & MP_PIXEL_BGR_ORDER) != 0u);
    CHECK((k_bgra8.flags & MP_PIXEL_ALPHA) != 0u);
    CHECK((k_bgra8.flags & MP_PIXEL_FLOAT) == 0u);

    CHECK((k_rgba16f.flags & MP_PIXEL_FLOAT) != 0u);
    CHECK((k_rgba16f.flags & MP_PIXEL_BGR_ORDER) == 0u);
    CHECK(k_rgba32f.container_bits == 32u);
    CHECK(k_rgb10a2.bits == 10u);
    CHECK((k_rgb10a2.flags & MP_PIXEL_FLOAT) == 0u);

    // Every one of them fits: `bits` plus `shift` inside `container_bits` is
    // the invariant a consumer is allowed to rely on.
    const MpPixelLayout* all[] = {&k_nv12,    &k_p010,     &k_bgra8,
                                  &k_rgba16f, &k_rgb10a2, &k_rgba32f};
    for (const MpPixelLayout* l : all) {
        CHECK(l->size == sizeof(MpPixelLayout));
        CHECK(l->bits + l->shift <= l->container_bits);
        CHECK(l->container_bits % 8u == 0u);
    }
}

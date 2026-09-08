// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kernels the presenter resamples with, named, and where a chroma sample
// sits. **Names and numbers only, no arithmetic**: the weights are computed
// in the shader, once per tap, and tests/scaler_test.cpp holds that shader to
// references written from the kernels' published formulas rather than to a
// C++ copy of the same code.
//
// Why there is a resampler at all, when a bilinear fetch had been doing the
// job: plan.md §9.7.1 put the scale in the presenter's own shader so that the
// filter would be ours to improve, and the day the HDR10 test patterns
// arrived was the day it needed improving. A 4K checkerboard bilinearly
// downscaled into a 1080p window is moiré the size of a thumb; a one-pixel
// ruler along the edge of a 2.39:1 field comes and goes with the phase; text
// in anything larger than the window is smeared. Two taps cannot average a
// neighbourhood, and averaging the neighbourhood is what downscaling is.

#ifndef MEDIAPERCH_VIDEO_KERNELS_HPP
#define MEDIAPERCH_VIDEO_KERNELS_HPP

#include <cstdint>
#include <cstring>

namespace mp::video {

/// The kernels, numbered as the shader's `kernel_weight` switches on them.
enum class Kernel : std::uint32_t {
    /// The triangle: two taps, and what the fetch was before there was a
    /// choice. Kept so that the old picture is one setting away.
    bilinear = 0,
    /// Lanczos with three lobes: a windowed sinc, the usual choice for both
    /// directions and the default here.
    lanczos = 1,
    /// The cubic spline with 36 support points, as ImageMagick and mpv spell
    /// it. Slightly less ringing than Lanczos at the same sharpness.
    spline36 = 2,
    /// Catmull-Rom: the cubic with B=0, C=1/2. Four taps, interpolating.
    catrom = 3,
    /// Mitchell-Netravali, B=C=1/3. Four taps and **not interpolating**: it
    /// blurs a picture even at one to one, by design, which is why it is not
    /// the default for anything.
    mitchell = 4,
    /// The box: an area average when downscaling by an integer factor, and
    /// nearest-neighbour when upscaling. What a checkerboard is measured with.
    box = 5,
};

/// Half the width of a kernel, in taps at one to one. Stretched by the
/// downscale factor when the picture is being made smaller, so that every
/// source sample under an output sample is counted.
[[nodiscard]] constexpr float support_of(Kernel k) noexcept
{
    switch (k) {
    case Kernel::bilinear:
        return 1.0f;
    case Kernel::lanczos:
    case Kernel::spline36:
        return 3.0f;
    case Kernel::catrom:
    case Kernel::mitchell:
        return 2.0f;
    case Kernel::box:
        return 0.5f;
    }
    return 3.0f;
}

[[nodiscard]] constexpr const char* name_of(Kernel k) noexcept
{
    switch (k) {
    case Kernel::bilinear:
        return "bilinear";
    case Kernel::lanczos:
        return "lanczos";
    case Kernel::spline36:
        return "spline36";
    case Kernel::catrom:
        return "catrom";
    case Kernel::mitchell:
        return "mitchell";
    case Kernel::box:
        return "box";
    }
    return "lanczos";
}

[[nodiscard]] inline bool kernel_from_name(const char* name, Kernel& out) noexcept
{
    constexpr Kernel all[] = {Kernel::bilinear, Kernel::lanczos, Kernel::spline36,
                              Kernel::catrom,   Kernel::mitchell, Kernel::box};
    for (const Kernel k : all) {
        if (name != nullptr && std::strcmp(name, name_of(k)) == 0) {
            out = k;
            return true;
        }
    }
    return false;
}

/// **Where a chroma sample sits against the luma grid**: ISO/IEC 23091-2's
/// `ChromaLocType`, the six positions H.273 draws for 4:2:0. Sample k of a
/// subsampled axis sits at luma sample 2k + offset, and the offsets are the
/// table:
///
///     type 0  left         (0,   0.5)   MPEG-2, H.264, HEVC and AV1's default
///     type 1  centre       (0.5, 0.5)   JPEG, and MPEG-1
///     type 2  top-left     (0,   0)     AV1's "colocated"
///     type 3  top          (0.5, 0)
///     type 4  bottom-left  (0,   1)
///     type 5  bottom       (0.5, 1)
///
/// For 4:2:2 only the horizontal half applies; 4:4:4 has no siting at all.
constexpr std::uint32_t k_siting_left = 0;
constexpr std::uint32_t k_siting_centre = 1;
constexpr std::uint32_t k_siting_types = 6;

[[nodiscard]] constexpr float siting_offset_x(std::uint32_t type) noexcept
{
    return (type % 2u) == 0u ? 0.0f : 0.5f;
}

[[nodiscard]] constexpr float siting_offset_y(std::uint32_t type) noexcept
{
    return type < 2u ? 0.5f : (type < 4u ? 0.0f : 1.0f);
}

[[nodiscard]] constexpr const char* siting_name(std::uint32_t type) noexcept
{
    switch (type) {
    case 0:
        return "left";
    case 1:
        return "centre";
    case 2:
        return "topleft";
    case 3:
        return "top";
    case 4:
        return "bottomleft";
    case 5:
        return "bottom";
    default:
        break;
    }
    return "left";
}

[[nodiscard]] inline bool siting_from_name(const char* name, std::uint32_t& out) noexcept
{
    for (std::uint32_t type = 0; type < k_siting_types; ++type) {
        if (name != nullptr && std::strcmp(name, siting_name(type)) == 0) {
            out = type;
            return true;
        }
    }
    // The British spelling above, and the other one too: a setting is typed.
    if (name != nullptr && std::strcmp(name, "center") == 0) {
        out = k_siting_centre;
        return true;
    }
    return false;
}

/// What a colour past the display's gamut gets.
enum class Gamut : std::uint32_t {
    /// Each component on its own, clipped to the display's range -- or rather
    /// left for the compositor to clip, which is what happened before there
    /// was a choice. A saturated BT.2020 red loses its gradation from about
    /// a third of the way up and every colour merges at a different level.
    clip = 0,
    /// Desaturated towards the achromatic axis at constant luminance, with a
    /// soft knee, so that a colour the display cannot show keeps its
    /// brightness and its hue and gives up saturation instead. The default.
    desaturate = 1,
};

[[nodiscard]] constexpr const char* name_of(Gamut g) noexcept
{
    return g == Gamut::clip ? "clip" : "desaturate";
}

[[nodiscard]] inline bool gamut_from_name(const char* name, Gamut& out) noexcept
{
    if (name != nullptr && std::strcmp(name, "clip") == 0) {
        out = Gamut::clip;
        return true;
    }
    if (name != nullptr && std::strcmp(name, "desaturate") == 0) {
        out = Gamut::desaturate;
        return true;
    }
    return false;
}

} // namespace mp::video

#endif // MEDIAPERCH_VIDEO_KERNELS_HPP

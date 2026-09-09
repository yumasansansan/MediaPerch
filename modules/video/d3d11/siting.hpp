// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a chroma sample sits, and what a colour past the display's gamut gets.
// **Names and numbers only, no arithmetic**: the shader does the work and the
// tests hold it to the standards' tables.
//
// The kernels used to live here too, until the scaler became a stage
// (§9.11): they are `modules/shared/kernels/kernels.hpp` now, shared with it,
// so that the presenter's chroma reconstruction and the stage's resampling
// compile one formula set.

#ifndef MEDIAPERCH_VIDEO_SITING_HPP
#define MEDIAPERCH_VIDEO_SITING_HPP

#include <cstdint>
#include <cstring>

namespace mp::video {

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

#endif // MEDIAPERCH_VIDEO_SITING_HPP

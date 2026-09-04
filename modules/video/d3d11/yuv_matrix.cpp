// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuv_matrix.hpp"

namespace mp::video {

namespace {

/// The luma weights a standard states, and everything else follows from them.
struct Weights {
    double kr;
    double kb;
};

Weights weights_for(std::uint32_t matrix) noexcept
{
    switch (matrix) {
    case 1u: // BT.709
        return {0.2126, 0.0722};
    case 4u: // FCC
        return {0.30, 0.11};
    case 5u: // BT.470 System B/G, which is BT.601 625-line by another name
    case 6u: // SMPTE 170M -- BT.601 525-line
    case 7u: // SMPTE 240M
        return {0.299, 0.114};
    case 9u:  // BT.2020 non-constant luminance
    case 10u: // BT.2020 constant luminance, whose *matrix* is the same three
              // weights; the difference is in how luma is formed, which is a
              // decoder's business rather than this one's
        return {0.2627, 0.0593};
    default:
        break;
    }
    // **Unspecified is BT.709, and only above standard definition.** Code point
    // 2 is what a container that says nothing leaves, and the caller has
    // already applied the resolution convention -- see `assumed_primaries`. A
    // matrix nobody named is BT.709 here because that is what every player
    // does with HD content and because guessing BT.601 for HD tints faces
    // green in a way people describe as "the colours are slightly off".
    return {0.2126, 0.0722};
}

} // namespace

YuvMatrix yuv_matrix_for(std::uint32_t matrix_code_point, bool full_range,
                         double sample_scale) noexcept
{
    const Weights w = weights_for(matrix_code_point);
    const double kr = w.kr;
    const double kb = w.kb;
    const double kg = 1.0 - kr - kb;

    YuvMatrix out{};
    // Every one of these is exact in double and rounded once, here.
    out.r_v = static_cast<float>(2.0 * (1.0 - kr));
    out.b_u = static_cast<float>(2.0 * (1.0 - kb));
    out.g_u = static_cast<float>(2.0 * kb * (1.0 - kb) / kg);
    out.g_v = static_cast<float>(2.0 * kr * (1.0 - kr) / kg);

    if (full_range) {
        out.luma_offset = 0.0f;
        out.luma_scale = 1.0f;
        out.chroma_scale = 1.0f;
    } else {
        // **Studio range, stated as fractions of the container rather than as
        // decimals.** 16..235 for luma and 16..240 for chroma at eight bits,
        // and the same fractions at ten -- 64..940 and 64..960 -- which is why
        // this is written as ratios and not as two tables.
        out.luma_offset = static_cast<float>(16.0 / 255.0);
        out.luma_scale = static_cast<float>(255.0 / 219.0);
        out.chroma_scale = static_cast<float>(255.0 / 224.0);
    }

    // See the header: ten bits sitting in the top of sixteen do not sample to
    // the value they name. Worked out from the layout by the caller, because
    // where a sample sits in its container is not this file's subject -- and
    // because two producers of the same depth put it in different places.
    out.sample_scale = static_cast<float>(sample_scale);
    return out;
}

} // namespace mp::video

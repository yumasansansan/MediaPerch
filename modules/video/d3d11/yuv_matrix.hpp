// SPDX-License-Identifier: GPL-3.0-or-later
//
// The YUV to RGB matrix, derived rather than tabulated.
//
// **Double precision on the CPU, single in the shader**, which is the rule
// §9.10 states and this is the first place it earns anything. Every one of
// these coefficients is a ratio of the luma weights, and the luma weights are
// three-decimal constants a standard states -- so the arithmetic is done in
// `double` once, at open, and rounded once into a constant buffer. Tabulating
// the results instead would mean transcribing eight numbers per matrix by hand,
// which is how a digit goes missing.
//
// The weights themselves are from ITU-R: BT.601 and BT.709 state Kr and Kb
// directly, BT.2020 states them for the non-constant-luminance form, and every
// other coefficient follows.

#ifndef MEDIAPERCH_VIDEO_YUV_MATRIX_HPP
#define MEDIAPERCH_VIDEO_YUV_MATRIX_HPP

#include <cstdint>

namespace mp::video {

/// What a shader needs to turn Y'CbCr into non-linear R'G'B'.
///
/// The form is the one every standard writes it in, because it has fewer
/// numbers than a 3x3 and each of them means something:
///
///     y = (Y - offset) * luma_scale
///     u = (Cb - 0.5)   * chroma_scale
///     v = (Cr - 0.5)   * chroma_scale
///     R = y                 + r_v * v
///     G = y - g_u * u - g_v * v
///     B = y + b_u * u
struct YuvMatrix {
    float luma_offset = 0.0f;
    float luma_scale = 1.0f;
    float chroma_scale = 1.0f;
    float r_v = 0.0f;
    float g_u = 0.0f;
    float g_v = 0.0f;
    float b_u = 0.0f;
    /// **What a sample has to be multiplied by to mean what it says.** NV12 in
    /// an `R8_UNORM` texture samples to exactly its value over 255, so this is
    /// 1. P010 puts ten bits in the *top* of each sixteen, so an `R16_UNORM`
    /// sample is `(v << 6) / 65535` and this is `65535 / (1023 * 64)` -- a
    /// factor of 1.00098, which is small, is not one, and is the difference
    /// between white and one part in a thousand under it.
    float sample_scale = 1.0f;
};

/// The matrix for an ISO/IEC 23091-2 matrix code point.
///
/// `full_range` is the flag `MpVideoInfo` carries: studio range puts Y in
/// 16..235 and chroma in 16..240 of an 8-bit container, and a decoder that
/// treated it as full range would crush the blacks and clip the whites -- which
/// looks like a contrast setting rather than like a bug.
[[nodiscard]] YuvMatrix yuv_matrix_for(std::uint32_t matrix_code_point, bool full_range,
                                       bool ten_bit) noexcept;

} // namespace mp::video

#endif // MEDIAPERCH_VIDEO_YUV_MATRIX_HPP

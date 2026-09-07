// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A Cube LUT, read from the text everyone writes them in.
//
// **The format is Adobe's and it is small**: a size, an optional domain, and
// then the table, one `R G B` triplet per line with red varying fastest. Every
// grading tool in use writes it and most of them read nothing else, so a
// lookup-table stage that read anything else would be a stage nobody could feed.
//
// **Here rather than in the module**, and portable, for the reason
// `modules/shared/h264` is: a parser is where the bugs are, and a parser that
// needs a GPU to run is a parser nobody fuzzes. Nothing in this file knows what
// Direct3D is; `modules/vdsp/lut` turns what comes out of it into a texture.
//
// **1D LUTs are refused rather than half-read.** A `LUT_1D_SIZE` file is a
// different transform -- three independent curves, not a cube -- and reading
// one as though it were a cube would be a colour transform nobody asked for.
// The refusal names it, so a person who fed one knows what they have.

#ifndef MEDIAPERCH_CUBE_HPP
#define MEDIAPERCH_CUBE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// A parsed cube.
struct CubeLut {
    /// Whatever `TITLE` said, or empty. Kept because a grader's LUTs are told
    /// apart by it and a settings row that could show it and did not would be
    /// a settings row about nothing.
    std::string title;
    /// Entries per axis. `size * size * size` triplets follow.
    std::uint32_t size = 0;
    /// `DOMAIN_MIN` and `DOMAIN_MAX`, per channel. The default is 0..1, which
    /// is what almost every file states and what the format assumes when it
    /// says nothing -- but a log-space LUT may state otherwise and a reader
    /// that ignored it would apply the table to the wrong range.
    float domain_min[3] = {0.0f, 0.0f, 0.0f};
    float domain_max[3] = {1.0f, 1.0f, 1.0f};
    /// `size^3` RGB triplets, **red varying fastest**, then green, then blue.
    /// That is the format's own order and it is also Direct3D's for a 3D
    /// texture, so the vector uploads without being rearranged.
    std::vector<float> table;

    /// Whether it is the transform that changes nothing.
    ///
    /// **Worth asking**, because it is the only assertion about a lookup table
    /// that needs no reference: an identity LUT applied to a picture must give
    /// that picture back, to the bit. `tolerance` is how far a value may sit
    /// from where the identity puts it -- exactly zero for a table written by
    /// something that computed it, and a little more for one that printed six
    /// decimal places.
    [[nodiscard]] bool identity(float tolerance = 0.0f) const noexcept;
};

/// What `parse_cube` made of a file.
struct CubeText {
    CubeLut lut;
    bool ok = false;
    /// Empty when `ok`. Otherwise what was wrong, with the line it was on --
    /// "the LUT would not parse" is not something a person can act on.
    std::string why;
};

/// Reads the text of a `.cube` file.
///
/// **Never throws and never trusts.** The size is bounded before anything is
/// allocated, because `LUT_3D_SIZE 4000000000` in a two-line file would
/// otherwise be a memory bomb with no data behind it -- and the bound is the
/// format's own: Adobe's specification says 2 to 256, and a table at 256 is
/// sixty-four megabytes of floats, which is already more than any real file.
[[nodiscard]] CubeText parse_cube(std::string_view text, std::string_view name = "the LUT");

/// The largest `LUT_3D_SIZE` this reads. The specification's own ceiling.
inline constexpr std::uint32_t k_cube_max_size = 256;

/// A cube that changes nothing, at `size` per axis. What a stage holds until it
/// is given a file, and what the identity test is written against.
[[nodiscard]] CubeLut identity_cube(std::uint32_t size);

} // namespace mp

#endif // MEDIAPERCH_CUBE_HPP

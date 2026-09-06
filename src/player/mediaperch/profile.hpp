// SPDX-License-Identifier: GPL-3.0-or-later
//
// **What this machine measured, in text, beside the settings file rather than
// in it.**
//
// §11's file is the user's: `[player]` keys are arguments to `Player::set` and a
// person edits them by hand. What a calibration writes is not a setting, it is
// machine-local measured state, and a program that rewrites the user's file is
// a program that fights the user's editor. So it lives in its own file, it
// supplies *defaults*, and any explicit setting beats it.
//
// The split is the same one §11 already made and this does not invent a second:
// **the head opens the file and this decides what the text means.** Nothing
// here touches a filesystem, so the Linux head gets it for free and a test can
// hand it a string.
//
// INI-shaped for the same reason the settings file is -- one parser,
// DragonPerch's, with one fuzz corpus. One `[measurement]` section per class of
// stream, repeated:
//
//     # HEVC 3840x2160
//     [measurement]
//     codec = 0x00000101
//     width = 3840
//     height = 2160
//     fps = 24000/1001
//     ring_ms = 96
//     threads = 8
//     runs = 3
//     file = clips/forest.mkv
//
// **The codec is its number and the comment above it is its name.** A second
// table of codec names would be a second place to forget the next codec in, and
// `result.cpp` already says so about the first; a number is total and never
// goes stale, and the name is written from the one table there is.
//
// A section that cannot be read is skipped and named, which is `OnBadLine::skip`
// applied to a whole section: a profile that loses one class to a typo beats one
// that loses every class, and both beat one that silently keeps a number nobody
// can account for.
//
// One inherited limitation: `#` and `;` start a comment anywhere on a line, so a
// path containing either cannot be written back. `write_profile` leaves such a
// path out rather than writing one that reads back as something else -- the
// measurement is still there, and only the note about where it came from is
// lost.

#ifndef MEDIAPERCH_PROFILE_HPP
#define MEDIAPERCH_PROFILE_HPP

#include "mediaperch/buffering.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// What `parse_profile` could not read, each naming its line, in the order the
/// file gave them.
struct ProfileText {
    Profile profile;
    std::vector<std::string> complaints;
};

/// Reads `text`. `name` is what a complaint calls the file.
[[nodiscard]] ProfileText parse_profile(std::string_view text, std::string_view name);

/// Writes what `parse_profile` reads. The round trip is a test.
[[nodiscard]] std::string write_profile(const Profile& profile);

} // namespace mp

#endif

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rate an encoder meant, when the container could only round it.
//
// **Not Matroska's problem alone, which is why this is not in refresh.hpp.**
// Any container that states a frame *duration* rather than a *rate*, in units
// that do not divide 1001, hands a reader a rounding: the ratio is recovered
// from the duration and the duration was recovered from the ratio. Matroska is
// the one this tree has measured -- `DefaultDuration` is whole nanoseconds, and
// 24000/1001 is 41708333.33 of them -- and any demuxer that derives a rate the
// same way lands in the same place. So the correction lives here, once, and
// every consumer of `MpVideoInfo::fps_num` calls it rather than growing its own.
//
// **The demuxers do not call it**, and that split is deliberate. A module
// reports what the file says: `1000000000/41708333` is what is in the file, and
// a demuxer that reported something else would be a demuxer whose output you
// could not check against a hex dump. Reading is the engine's, and this is
// where the engine reads.
//
// **It is not a guess, and the numbers are the argument.** Over the rates a
// container carries, the largest error the rounding imposes is 4.0e-8 of the
// rate; the closest two candidates -- every 1000/1001 pair, 23.976 against 24 --
// are 1.0e-3 apart. Twenty-five thousand times the margin. Inverting a lossy
// encoding whose candidates are separated by five orders of magnitude more than
// its error is decoding, not overruling the file.
//
// And a rate that is genuinely none of them stays as it is: 23.98 exactly lands
// 1.7e-4 away from 23.976, four thousand times the rounding, and comes back
// untouched. The threshold is 1e-6 -- twenty-five times above the worst
// rounding, eighty times below the nearest real rate that is not on the list.
//
// What it cannot repair is a container that rounded *coarsely*. An MP4 written
// with a timescale of 1000 stores a 23.976 fps frame as 42 milliseconds, which
// reads back as 23.810: six parts in a thousand out, past any threshold that
// still tells 23.976 from 24, and genuinely ambiguous. That file is left alone
// and is not usable for matching a refresh rate either.

#ifndef MEDIAPERCH_FRAMERATE_HPP
#define MEDIAPERCH_FRAMERATE_HPP

#include "mediaperch/rational.hpp"

#include <span>

namespace mp {

/// The rates the correction knows, ordered by rate.
[[nodiscard]] std::span<const Rational> standard_frame_rates() noexcept;

/// `stated` as the encoder meant it, or `stated` unchanged.
///
/// `snapped` says which happened when a caller passes one, because a program
/// that quietly improves its input is a program nobody can debug. A rate that
/// is already one of the standard ones is returned as it came, with `snapped`
/// false: an MP4 that said 24000/1001 is not a rounding of anything.
[[nodiscard]] Rational snap_frame_rate(Rational stated, bool* snapped = nullptr) noexcept;

} // namespace mp

#endif // MEDIAPERCH_FRAMERATE_HPP

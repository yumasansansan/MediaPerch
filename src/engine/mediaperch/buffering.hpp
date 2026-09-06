// SPDX-License-Identifier: GPL-3.0-or-later
//
// **What a measurement is allowed to conclude, and what it may not.**
//
// §9.8.2 measured the ring: 4K HEVC beside audio underran at 8 device periods
// and did not at 16, the ring's low-water mark stops moving above 16, and the
// reachable ring sizes are powers of two because `ByteRing` rounds up to one.
// The default is 128 periods, which is generous **against that material** and
// says nothing whatever about material bigger than it. 16K60 beside f64 PCM
// would make 683 ms a small ring, and no adjective in a comment changes that.
//
// **So a measurement is not capped here.** This returned `min(measured,
// default)` for one revision, on the argument that a profile should only be
// able to talk the ring down; that argument is wrong twice. It throws away the
// one case where the profile knows something the default cannot -- a class that
// needs *more* -- and leaves that class glitching while the answer sits in a
// file. And it decides for somebody: §11 took a `2..4096` range off
// `ring_periods` for exactly this reason and said what to do instead, which is
// that a ring the machine cannot allocate comes back as a run that failed,
// caught where the graph is built, rather than as a number refused in advance.
//
// What is still true is which direction is dangerous. Too much ring is bytes
// and a slower start, both bounded; too little is a click. That asymmetry is
// why an unmeasured class gets the default and why an answer measured on
// heavier material may stand in for lighter material. It is not a reason to
// overrule a measurement.
//
// **This is where the answer is needed, which is why it is in the engine.**
// The ring is chosen where the graph is built, and the engine builds the graph:
// a policy in a shell would leave `mediaperchd` with no shell attached (§10's
// tray-only install) and anything embedding `src/engine` -- a colour grader, an
// editor -- with no policy at all. It is pure arithmetic, so it is tested
// without a device, the way `refresh.hpp` and `framerate.hpp` are.

#ifndef MEDIAPERCH_BUFFERING_HPP
#define MEDIAPERCH_BUFFERING_HPP

#include <mediaperch/module.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// **What a calibration is allowed to move**, chosen by the person running it.
///
/// A calibration cannot be faster than real time. The quantity is whether the
/// decode thread keeps the ring full against the device's deadline, and a run
/// with no device has no deadline and measures nothing -- so measuring costs
/// exactly as long as the material, with the sound audible. Which of these is
/// worth that is not a question this program can answer for somebody, so it
/// does not: it asks.
enum class Dimension : std::uint32_t {
    none = 0,
    /// `PassthroughConfig::ring_periods`.
    ring = 1u << 0,
    /// How many threads a video decoder is given. Multiplies the run time by
    /// the number of counts tried, on top of the ring's own sweep.
    decoder_threads = 1u << 1,
};

constexpr Dimension operator|(Dimension a, Dimension b) noexcept
{
    return static_cast<Dimension>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr Dimension operator&(Dimension a, Dimension b) noexcept
{
    return static_cast<Dimension>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

constexpr bool has(Dimension set, Dimension one) noexcept
{
    return (set & one) != Dimension::none;
}

/// The name a user types for one dimension, and back. Empty name for anything
/// that is not one, rather than a guess.
[[nodiscard]] std::string_view dimension_name(Dimension one) noexcept;
[[nodiscard]] bool dimension_from_name(std::string_view name, Dimension& out) noexcept;

/// **The part of a file a buffering answer depends on**, all of it stated by the
/// container before anything is decoded.
///
/// Not bit depth and not chroma: ABI v4 puts those on the *frame*, so a
/// container does not state them and neither can anything asking this question
/// before it opens a decoder. What is left is enough, because what the ring has
/// to cover is the cost of one frame and how often that cost recurs.
struct StreamShape {
    MpCodec codec = MP_CODEC_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 0;
    std::uint32_t fps_den = 0;

    friend bool operator==(const StreamShape& a, const StreamShape& b) noexcept
    {
        return a.codec == b.codec && a.width == b.width && a.height == b.height &&
               a.fps_num == b.fps_num && a.fps_den == b.fps_den;
    }
};

/// Luma samples per second, which is the axis a decode cost is ordered on.
///
/// Zero when the shape does not say -- a container that timestamps every frame
/// instead of stating a rate leaves `fps_num`/`fps_den` at 0/0 -- and a shape
/// with no cost matches nothing, which is the safe end of not knowing.
[[nodiscard]] double samples_per_second(const StreamShape& shape) noexcept;

/// **What one calibration concluded about one class of stream.** Milliseconds,
/// not periods and not bytes: a period is 3 ms on one device and four times
/// that on another, so periods do not survive being written down.
struct Measurement {
    StreamShape shape;
    /// The ring this class needed, in milliseconds of audio, with whatever
    /// margin the calibration decided to keep. What a policy reads.
    double ring_ms = 0.0;
    /// Video decoder threads this class wanted. 0 when the calibration was not
    /// asked to move that dimension.
    std::uint32_t decoder_threads = 0;
    /// **What it rests on.** The file, and how many runs agreed. Every other
    /// measurement in this tree is kept with its inputs; this one becomes a
    /// setting, so it matters more rather than less -- a person reading the
    /// profile can see what the answer was measured on and disagree with it.
    std::string file;
    std::uint32_t runs = 0;
};

/// Everything a machine has been told to measure, in no particular order.
struct Profile {
    std::vector<Measurement> measured;
};

/// The measurement that answers for `shape`, or null.
///
/// **The cheapest class measured that is still at least as expensive as this
/// one.** An answer measured on harder material is safe for easier material and
/// merely wasteful, which is the direction this file is willing to be wrong in.
/// An exact match wins outright. Nothing at or above it means no answer, and no
/// answer means the default -- never an extrapolation, because the class above
/// the largest one measured is exactly where a model would be guessing.
[[nodiscard]] const Measurement* answer_for(const StreamShape& shape,
                                            const Profile& profile) noexcept;

/// **The ring for `shape`, in device periods.** Larger than `fallback` when
/// that is what was measured, and smaller when that is.
///
/// `fallback` is what to use when nothing has been measured for this class, not
/// a ceiling: a measurement that asks for more than the default is the case the
/// default is least able to answer, and refusing it here would leave the class
/// glitching with its answer already written down. A size the machine cannot
/// allocate is reported by the run that tried it.
///
/// `period_frames` and `sample_rate` are the device's, because that is what
/// turns the profile's milliseconds back into periods.
[[nodiscard]] std::uint32_t ring_for(const StreamShape& shape, const Profile& profile,
                                     std::uint32_t period_frames, std::uint32_t sample_rate,
                                     std::uint32_t fallback) noexcept;

} // namespace mp

#endif

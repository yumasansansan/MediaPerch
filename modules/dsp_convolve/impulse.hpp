// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading an impulse response, and the four things that have to happen to one
// before it can be convolved with anything.
//
// **A measurement is not a filter until it agrees with the audio.** An impulse
// response arrives as a recording -- of a room, of a speaker, of a headphone
// compensation -- at whatever rate and channel count somebody measured it at,
// with whatever gain their microphone happened to have. Four questions follow,
// and this file answers each of them out loud rather than quietly:
//
//   the rate       An impulse measured at 44.1 kHz applied to a 96 kHz stream
//                  is the wrong filter -- every feature of it lands an octave
//                  and a bit from where it belongs. It is **resampled**, using
//                  the resampler this tree already has, because that is exactly
//                  the same decision the equaliser makes when it re-derives its
//                  biquads at each rate rather than transcribing them at one.
//   the channels   One response is applied to every channel; one per channel is
//                  applied one to one. Anything else is refused, because a
//                  four-channel file is a true-stereo matrix and that is a
//                  different convolution.
//   the length     A ten-second cathedral is half a million taps. `max_taps`
//                  truncates, with a fade so the cut does not become a click.
//   the gain       Reported always and changed only when asked. A room
//                  correction is already at the level its author meant; a
//                  reverb usually is not.

#ifndef MEDIAPERCH_IMPULSE_HPP
#define MEDIAPERCH_IMPULSE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mp::impulse {

/// Deinterleaved, one plane per channel, as read.
struct Response {
    std::vector<std::vector<double>> channels;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] std::size_t frames() const noexcept
    {
        return channels.empty() ? 0 : channels.front().size();
    }
    [[nodiscard]] bool empty() const noexcept { return frames() == 0; }
};

/// What a caller needs to know before deciding whether to change it.
struct Gains {
    /// The response at zero hertz: the sum of the taps. What a steady signal
    /// gets multiplied by, and the number that decides whether a correction
    /// makes everything louder.
    double dc = 1.0;
    /// The largest single tap.
    double peak = 0.0;
    /// The square root of the sum of squares: what an uncorrelated signal gets.
    double energy = 0.0;
};

/// Reads a WAV (and everything else dr_wav reads: RF64, W64, AIFF).
///
/// The path is UTF-8, as the ABI says. Samples come back as `float` from the
/// reader and are widened -- which loses nothing, because a 24-bit measurement
/// has 24 bits of mantissa and a 32-bit float has 24.
[[nodiscard]] bool load(const std::string& path, Response& out, std::string& why);

[[nodiscard]] Gains measure(const Response& response) noexcept;
void scale(Response& response, double factor) noexcept;

/// Truncates to `max_taps`, fading the last hundredth so the cut is a fade and
/// not a step. Does nothing when the response is already shorter.
void truncate(Response& response, std::size_t max_taps) noexcept;

/// Resamples the response to `rate`. Uses the tree's own resampler at its
/// highest setting: this happens once, at configure, on a few hundred thousand
/// samples, so there is no reason for it to be anything less.
[[nodiscard]] bool resample_to(Response& response, std::uint32_t rate, std::string& why);

/// Makes the response fit a stream of `channels`: one response is broadcast,
/// `channels` responses are taken one to one, and anything else is refused.
[[nodiscard]] bool fit_channels(Response& response, std::uint32_t channels,
                                std::string& why);

} // namespace mp::impulse

#endif // MEDIAPERCH_IMPULSE_HPP

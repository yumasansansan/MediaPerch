// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one place in this program where a sample is allowed to change.
//
// Everything else -- the decoders, the repack, the whole passthrough graph --
// exists to move bytes without touching them. This does the opposite, on
// purpose, where the user asked for it and where it can be seen. It is the
// front half of Path B.
//
// What it does and does not do is the important part:
//
//   * **Sample type**, in either direction, through a normalised double. This is
//     what a float decoder needs to reach a device that takes integers, which
//     is the case that made Path B necessary rather than theoretical.
//   * **Gain**, because a volume control on an exclusive-mode stream has
//     nowhere else to live: the session interfaces do not touch it.
//   * **Dither**, when the result is an integer narrower than the signal.
//     Rounding without it is a lie of about half an LSB, correlated with the
//     signal, and correlated error is what an ear hears as distortion rather
//     than noise.
//
// It does **not** resample and does **not** change the channel count. Both are
// real conversions that need a real implementation, and a bad one here would be
// worse than the refusal it replaced. Negotiation keeps the rate and the layout
// fixed, so `possible()` is a check on that rather than a promise to try.
//
// Runs on the decode thread. Never on the render thread.

#ifndef MEDIAPERCH_CONVERT_HPP
#define MEDIAPERCH_CONVERT_HPP

#include "mediaperch/dither.hpp"
#include "mediaperch/format.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp {

struct ConvertConfig {
    /// Linear, not decibels: 1.0 is unity and the graph does no arithmetic on it.
    double gain = 1.0;

    /// Which distribution the dither is drawn from, applied only when the
    /// destination is an integer that cannot hold the source exactly.
    /// `DitherKind::none` is what a measurement wants; the rest are what
    /// listening wants, and they differ from each other -- see dither.hpp.
    DitherKind dither = DitherKind::triangular;

    /// Where the quantisation noise is put. Order 0 leaves it where it fell.
    NoiseShaping shaping{};

    /// The dither generator's seed. Fixed rather than from a clock, so two runs
    /// of the same file produce the same bytes and a difference means something.
    std::uint32_t seed = 0x1f2e3d4cu;
};

/// Turns frames of one format into frames of another.
///
/// Holds only its own state -- the formats, the gain and the dither generator --
/// and allocates nothing after construction.
class Converter {
public:
    Converter(const Format& from, const Format& to, ConvertConfig config = {}) noexcept;

    /// False when the pair needs something this does not do: a different rate,
    /// a different channel count, a non-PCM encoding, or a sample type with no
    /// numeric meaning. `run` is a no-op in that case rather than a guess.
    [[nodiscard]] bool possible() const noexcept { return possible_; }

    /// True when the conversion cannot represent the source exactly, which is
    /// every narrowing and every float-to-integer. Worth reporting: it is the
    /// difference between "the device took a different container" and "the
    /// signal was altered".
    [[nodiscard]] bool lossy() const noexcept { return lossy_; }

    /// `frames` frames from `src` in the source format to `dst` in the
    /// destination format. The buffers may not overlap.
    void run(const void* src, void* dst, std::size_t frames) noexcept;

    [[nodiscard]] const Format& from() const noexcept { return from_; }
    [[nodiscard]] const Format& to() const noexcept { return to_; }

    /// What is actually happening to the samples, in the words the settings
    /// use. For whoever has to tell the user, which §6.3 says somebody must.
    [[nodiscard]] DitherKind dither_kind() const noexcept { return config_.dither; }
    [[nodiscard]] std::uint32_t shaping_order() const noexcept;
    /// True when the destination is narrow enough that dither and shaping are
    /// doing something. Widening cannot lose anything, so neither runs.
    [[nodiscard]] bool quantising() const noexcept { return quantising_; }

private:
    Format from_;
    Format to_;
    ConvertConfig config_;
    bool possible_ = false;
    bool lossy_ = false;
    /// Full-scale magnitude of the destination, and the largest value that fits.
    double scale_ = 1.0;
    double ceiling_ = 1.0;
    double floor_ = -1.0;
    /// One least-significant bit of the destination, in destination units: 1 for
    /// a container whose valid bits fill it, 256 for 24 bits inside four bytes.
    double step_ = 1.0;
    bool quantising_ = false;

    /// One per channel. A shaper shared between channels feeds each channel's
    /// error into the others, which correlates their noise floors and puts a
    /// centre image on what should be two independent ones.
    std::vector<Dither> dither_;
};

} // namespace mp

#endif // MEDIAPERCH_CONVERT_HPP

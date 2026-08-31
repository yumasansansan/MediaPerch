// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC-LC, decoded here rather than by anything this project links to.
//
// The reason is narrower than it was for ALAC, and worth stating plainly. AAC
// is not an abandoned format and FAAD2 is not abandoned code; the problem was
// that every library available produced the wrong *thing* for this player:
//
//   * Media Foundation starts every AAC track 1024 frames -- 21.3 ms -- late,
//     leaves the encoder padding on the end, and refuses 8 kHz and 7.1 outright.
//   * FAAD2 discards two frames where the file says to discard one, so its
//     output begins 1024 frames into the audio. Four different ways of driving
//     it produced the identical wrong placement.
//   * libxaac is Apache-2.0, maintained, in OSS-Fuzz and shipped in Android --
//     and emits 16- or 24-bit integers only. AAC's inverse transform produces
//     real numbers; taking integers from a decoder is a quantisation performed
//     where nobody can see it.
//
// So this decoder exists to produce **float, framed exactly as the container
// says**. Both of those are decisions rather than arithmetic, and they are what
// the libraries got wrong.
//
// Scope is AAC-LC. SBR (HE-AAC) and PS (HE-AACv2) are not here and are not
// planned: they are another six thousand lines apiece, and a file that needs
// them falls through to decode_ffmpeg, which is what the fallback chain is for.
//
// No OS headers, no allocation during decode, no I/O.

#ifndef MEDIAPERCH_AAC_HPP
#define MEDIAPERCH_AAC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp::aac {

inline constexpr unsigned k_max_channels = 8;
inline constexpr unsigned k_frame_len = 1024;
inline constexpr unsigned k_short_len = 128;
inline constexpr unsigned k_windows = 8;
inline constexpr unsigned k_max_sfb = 51;

/// The AudioSpecificConfig, as far as AAC-LC needs it.
struct Config {
    unsigned object_type = 0;    ///< 2 is AAC-LC, and the only one accepted
    unsigned rate_index = 0;     ///< index into the standard's 13 rates
    std::uint32_t sample_rate = 0;
    unsigned channel_config = 0; ///< 0 means "read a program config element"
    bool frame_960 = false;      ///< frameLengthFlag; 960-sample frames
};

bool parse_asc(const std::uint8_t* asc, std::size_t bytes, Config& out) noexcept;

/// The sample rate for one of the standard's 13 indices, or 0.
std::uint32_t rate_for_index(unsigned index) noexcept;

/// One channel's worth of a decoded frame, before the filterbank.
struct Channel {
    unsigned window_sequence = 0; ///< ONLY_LONG, LONG_START, EIGHT_SHORT, LONG_STOP
    unsigned window_shape = 0;    ///< 0 sine, 1 KBD
    unsigned max_sfb = 0;
    unsigned num_window_groups = 1;
    unsigned group_len[k_windows] = {1};
    float coeffs[k_frame_len] = {}; ///< dequantised spectrum, grouped
};

class Decoder {
public:
    bool init(const Config& cfg) noexcept;

    /// Parses one raw_data_block and dequantises its spectrum.
    ///
    /// Returns false on anything malformed. On success `channels()` is how many
    /// channels the frame carried and `channel(i)` is each one's spectrum.
    bool decode_frame(const std::uint8_t* packet, std::size_t bytes) noexcept;

    [[nodiscard]] unsigned channels() const noexcept { return channels_; }
    [[nodiscard]] const Channel& channel(unsigned i) const noexcept { return chan_[i]; }
    [[nodiscard]] const char* error() const noexcept { return error_; }

    /// Bits the last frame consumed, and the bits it was given. A frame that
    /// parsed correctly ends inside the last byte of its packet; a parser that
    /// has gone wrong almost always ends somewhere else, which makes this the
    /// cheapest correctness check there is.
    [[nodiscard]] std::size_t last_bits_used() const noexcept { return bits_used_; }
    [[nodiscard]] std::size_t last_bits_given() const noexcept { return bits_given_; }

private:
    Config cfg_{};
    bool ready_ = false;
    const char* error_ = "";
    unsigned channels_ = 0;
    std::size_t bits_used_ = 0;
    std::size_t bits_given_ = 0;
    Channel chan_[k_max_channels];
};

} // namespace mp::aac

#endif // MEDIAPERCH_AAC_HPP

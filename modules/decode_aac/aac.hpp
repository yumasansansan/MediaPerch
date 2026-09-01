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
#include <memory>

namespace mp::aac {

inline constexpr unsigned k_max_channels = 8;
inline constexpr unsigned k_frame_len = 1024;
inline constexpr unsigned k_short_len = 128;
inline constexpr unsigned k_windows = 8;
inline constexpr unsigned k_max_sfb = 51;
inline constexpr unsigned k_max_tns_order = 20;

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

/// Channels in the order AAC's elements produce them, mapped onto WAVE slots.
///
/// A decoder hands channels back in the order their elements appeared, which for
/// 5.1 is C, L, R, Ls, Rs, LFE -- centre first, because the single-channel
/// element comes first. WAVE wants L, R, C, LFE, Ls, Rs. Without this table a
/// 5.1 decode measures -3 dB against a reference and sounds like the centre
/// channel wandered, which is exactly what Media Foundation does to ALAC.
struct ChannelLayout {
    std::uint32_t mask;   ///< MP_SPEAKER_* bits, 0 for mono and stereo
    std::uint8_t count;   ///< 0 if this configuration has no layout here
    std::uint8_t from[8]; ///< for each WAVE slot, the decoded channel that fills it
};

const ChannelLayout& layout_for_config(unsigned channel_config) noexcept;

/// Everything a frame needs while it is being decoded, held by the object
/// rather than by the file.
///
/// It is a lot of memory -- eight channels of spectrum, scale factors, codebook
/// assignments and TNS state -- which is why it is behind a pointer instead of
/// inline. Making it file-static would have been smaller and would have meant
/// two Decoders on two threads quietly corrupting each other.
struct State;

class Decoder {
public:
    Decoder() noexcept;
    ~Decoder() noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool init(const Config& cfg) noexcept;

    /// Decodes one raw_data_block into `k_frame_len` samples per channel.
    ///
    /// Returns false on anything malformed. On success `channels()` is how many
    /// channels the frame carried and `pcm(i)` is each one's output, in the
    /// order the elements appeared.
    bool decode_frame(const std::uint8_t* packet, std::size_t bytes) noexcept;

    [[nodiscard]] unsigned channels() const noexcept { return channels_; }
    [[nodiscard]] const float* pcm(unsigned channel) const noexcept { return pcm_[channel]; }
    [[nodiscard]] const char* error() const noexcept { return error_; }

    /// The window sequence the last frame used for a channel: 0 ONLY_LONG,
    /// 1 LONG_START, 2 EIGHT_SHORT, 3 LONG_STOP. Diagnostic, and the first thing
    /// to look at when one frame in a file is wrong and the rest are not.
    [[nodiscard]] unsigned window_sequence(unsigned channel) const noexcept
    {
        return last_sequence_[channel];
    }

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

    /// The second half of the previous frame's windowed output, per channel.
    /// AAC's transform is lapped: a frame is only finished once the next one
    /// has been added to it.
    float overlap_[k_max_channels][k_frame_len] = {};
    unsigned prev_shape_[k_max_channels] = {};
    float pcm_[k_max_channels][k_frame_len] = {};
    unsigned last_sequence_[k_max_channels] = {};
    /// The noise generator for PNS bands. Its state runs across frames, so a
    /// decode is reproducible from the start of a track and not from anywhere
    /// else -- which is true of AAC anyway, because the transform is lapped.
    std::uint32_t rng_ = 0x1f2e3d4cu;
    std::unique_ptr<State> state_;
};

} // namespace mp::aac

#endif // MEDIAPERCH_AAC_HPP

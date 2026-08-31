// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mediaperch/module.h>

#include <cstdint>
#include <string>

namespace mp {

enum class SampleType : std::uint32_t {
    none = MP_SAMPLE_NONE,
    s16 = MP_SAMPLE_S16,
    s24_packed = MP_SAMPLE_S24_PACKED,
    s24_in_32 = MP_SAMPLE_S24_IN_32,
    s32 = MP_SAMPLE_S32,
    f32 = MP_SAMPLE_F32,
};

enum class Encoding : std::uint32_t {
    pcm = MP_ENCODING_PCM,
    dop = MP_ENCODING_DOP,
    iec61937 = MP_ENCODING_IEC61937,
};

/// What the graph passes around, and what negotiation argues about.
///
/// `valid_bits` is the field that makes a widened candidate honest: 16-bit
/// content offered to a 24-in-32 endpoint keeps `valid_bits == 16`, so the host
/// can still say truthfully that no signal was lost.
struct Format {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t channel_mask = 0; ///< 0 = the non-extensible form
    SampleType sample_type = SampleType::none;
    Encoding encoding = Encoding::pcm;
    std::uint32_t valid_bits = 0; ///< 0 = all of the container

    friend bool operator==(const Format&, const Format&) = default;
};

/// Bytes one sample of this type occupies in a buffer.
[[nodiscard]] std::uint32_t container_bytes(SampleType type) noexcept;

/// Bits of that container which carry signal when nothing says otherwise.
[[nodiscard]] std::uint32_t natural_valid_bits(SampleType type) noexcept;

/// `valid_bits` if set, the natural width otherwise.
[[nodiscard]] std::uint32_t effective_valid_bits(const Format& f) noexcept;

[[nodiscard]] std::uint32_t frame_bytes(const Format& f) noexcept;
[[nodiscard]] std::uint64_t bytes_per_second(const Format& f) noexcept;

/// Rejects the formats no sink could ever be asked for: zero rate, zero
/// channels, no sample type, or more valid bits than the container holds.
[[nodiscard]] bool is_valid(const Format& f) noexcept;

/// The conventional speaker mask for a channel count, or 0 when there is no
/// single obvious answer. Used to build the extensible-form candidate.
[[nodiscard]] std::uint32_t conventional_channel_mask(std::uint32_t channels) noexcept;

/// For logs and for the UI: "44100 Hz / 2 ch / S24_IN_32 (24 valid)".
[[nodiscard]] std::string describe(const Format& f);

[[nodiscard]] MpFormat to_abi(const Format& f) noexcept;
[[nodiscard]] Format from_abi(const MpFormat& f) noexcept;

} // namespace mp

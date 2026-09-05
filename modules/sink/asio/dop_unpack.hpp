// SPDX-License-Identifier: GPL-3.0-or-later
//
// DoP back to DSD: the inverse of what `codec_dsd` did, and the one piece of
// `sink_asio` that can be checked without a driver.
//
// **The graph carries DSD as DoP** -- two DSD bytes in a 24-bit frame under an
// alternating 0x05/0xFA marker -- because that is what a PCM link can carry.
// An ASIO driver in DSD mode is not a PCM link, so the sink takes the framing
// back off. That is three lines of arithmetic and it is the only place in the
// DSD path where a mistake would be inaudible on a bench and wrong in a DAC:
// swap the two bytes and every sample is delayed by half a byte, which sounds
// like the same music slightly wrong rather than like nothing.
//
// So it lives here, in a header with no Windows in it, and `tests/dop_test.cpp`
// holds it against the layout `codec_dsd` writes.

#ifndef MEDIAPERCH_SINK_ASIO_DOP_UNPACK_HPP
#define MEDIAPERCH_SINK_ASIO_DOP_UNPACK_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace mp::asio {

/// DSD bytes carried by one DoP frame of one channel.
inline constexpr std::size_t k_dsd_bytes_per_dop_frame = 2;
/// Bytes one DoP frame of one channel occupies: a packed 24-bit sample.
inline constexpr std::size_t k_dop_frame_bytes = 3;

/// Reverses the bits of a byte, for a driver that wants DSD least significant
/// bit first. `demux_dsd` has the same table for the same reason, on the way in.
inline constexpr std::array<std::uint8_t, 256> make_reversed()
{
    std::array<std::uint8_t, 256> table{};
    // Walked rather than indexed: the analyzer could not follow a loop bound
    // through the inner loop and reported every store as possibly out of
    // range, and a table filled slot by slot has no index to be wrong about.
    std::uint8_t value = 0;
    for (std::uint8_t& slot : table) {
        std::uint8_t remaining = value;
        std::uint8_t reversed = 0;
        for (int bit = 0; bit < 8; ++bit) {
            reversed = static_cast<std::uint8_t>((reversed << 1) | (remaining & 1u));
            remaining = static_cast<std::uint8_t>(remaining >> 1);
        }
        slot = reversed;
        ++value; // 255 wraps to 0 on the last pass, and nothing reads it after
    }
    return table;
}
inline constexpr std::array<std::uint8_t, 256> k_reversed = make_reversed();

/// One channel's DSD bytes out of `frames` interleaved DoP frames.
///
/// `src` is `frames * channels` DoP frames, interleaved as the graph writes
/// them; `dst` receives `frames * 2` DSD bytes for channel `channel` alone,
/// which is the layout ASIO wants -- one buffer per channel.
///
/// **The marker is dropped rather than checked.** It carries no audio and a
/// frame that had the wrong one would still hold the right DSD; what would be
/// wrong is the stream a *PCM* DAC saw, and this is not one.
inline void unpack_channel(const std::uint8_t* src, std::size_t channels, std::size_t channel,
                           std::size_t frames, bool lsb_first, std::uint8_t* dst) noexcept
{
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint8_t* frame = src + (f * channels + channel) * k_dop_frame_bytes;
        // Packed little-endian 24-bit: the least significant byte first, so the
        // frame is [later, earlier, marker] and time runs the other way.
        std::uint8_t earlier = frame[1];
        std::uint8_t later = frame[0];
        if (lsb_first) {
            earlier = k_reversed[earlier];
            later = k_reversed[later];
        }
        dst[f * k_dsd_bytes_per_dop_frame] = earlier;
        dst[f * k_dsd_bytes_per_dop_frame + 1] = later;
    }
}

} // namespace mp::asio

#endif // MEDIAPERCH_SINK_ASIO_DOP_UNPACK_HPP

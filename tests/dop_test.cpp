// SPDX-License-Identifier: GPL-3.0-or-later
//
// DoP, unpacked -- the one part of `sink_asio` that runs without a driver.
//
// A DSD file reaches a DAC by one of two links and the difference is entirely
// here. Over WASAPI the DoP frames go out as they are, and `tools/make_dsd.py
// --check` is what proves the bits survived; over ASIO in DSD mode the sink
// takes the framing off first, and nothing else in the tree would notice if it
// took it off wrongly. A DAC would: the two DSD bytes of a frame are in time
// order, so swapping them delays every sample by half a byte, which is not
// silence and not noise but the same music slightly wrong.
//
// So this holds the unpack against the layout `codec_dsd` writes, stated here
// independently rather than taken from it.

#include <dop_unpack.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

/// The layout `codec_dsd` produces, written out again: for each frame, for each
/// channel, a packed 24-bit little-endian sample whose bytes are the later DSD
/// byte, the earlier one, and the marker.
std::vector<std::uint8_t> pack(const std::vector<std::vector<std::uint8_t>>& channels)
{
    const std::size_t n = channels.size();
    const std::size_t frames = channels.front().size() / 2;
    std::vector<std::uint8_t> out(frames * n * 3);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint8_t marker = (f % 2 == 0) ? 0x05 : 0xFA;
        for (std::size_t c = 0; c < n; ++c) {
            std::uint8_t* at = out.data() + (f * n + c) * 3;
            at[0] = channels[c][f * 2 + 1]; // later
            at[1] = channels[c][f * 2];     // earlier
            at[2] = marker;
        }
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> two_channels(std::size_t bytes_each)
{
    std::vector<std::vector<std::uint8_t>> out(2);
    for (std::size_t c = 0; c < 2; ++c) {
        out[c].resize(bytes_each);
        for (std::size_t i = 0; i < bytes_each; ++i) {
            out[c][i] = static_cast<std::uint8_t>(i * 37 + c * 101 + 11);
        }
    }
    return out;
}

} // namespace

TEST_CASE("every DSD byte comes back, in order, in the channel it went in as")
{
    const auto channels = two_channels(64);
    const auto dop = pack(channels);
    const std::size_t frames = 32;

    for (std::size_t c = 0; c < 2; ++c) {
        std::vector<std::uint8_t> got(frames * 2);
        mp::asio::unpack_channel(dop.data(), 2, c, frames, false, got.data());
        INFO("channel " << c);
        CHECK(got == channels[c]);
    }
}

TEST_CASE("the two bytes of a frame are in time order, which is the mistake to catch")
{
    // One frame, one channel: DSD bytes 0xAA then 0xBB.
    const std::vector<std::uint8_t> dop{0xBB, 0xAA, 0x05};
    std::vector<std::uint8_t> got(2);
    mp::asio::unpack_channel(dop.data(), 1, 0, 1, false, got.data());
    CHECK(got[0] == 0xAA); // the earlier sample, which the frame holds second
    CHECK(got[1] == 0xBB);
}

TEST_CASE("the marker is dropped and never reaches the driver")
{
    // Both markers, and neither should appear in the output. A DoP frame's top
    // byte is 0x05 or 0xFA; DSD bytes of those values are perfectly ordinary,
    // so this checks positions rather than values.
    const std::vector<std::uint8_t> dop{0x01, 0x02, 0x05, 0x03, 0x04, 0xFA};
    std::vector<std::uint8_t> got(4);
    mp::asio::unpack_channel(dop.data(), 1, 0, 2, false, got.data());
    CHECK(got == std::vector<std::uint8_t>{0x02, 0x01, 0x04, 0x03});
}

TEST_CASE("a driver that wants the least significant bit first gets it reversed")
{
    const std::vector<std::uint8_t> dop{0b0000'0001, 0b1000'0000, 0x05};
    std::vector<std::uint8_t> got(2);
    mp::asio::unpack_channel(dop.data(), 1, 0, 1, true, got.data());
    // 0x80 reversed is 0x01, and 0x01 reversed is 0x80.
    CHECK(got[0] == 0b0000'0001);
    CHECK(got[1] == 0b1000'0000);

    // And the table is a reversal, which is what that rests on.
    for (int i = 0; i < 256; ++i) {
        const auto v = static_cast<std::uint8_t>(i);
        CHECK(mp::asio::k_reversed[mp::asio::k_reversed[v]] == v);
    }
}

TEST_CASE("channels are separated, which is what ASIO's per-channel buffers need")
{
    // Six channels, so a channel picked out of the middle exercises the stride.
    std::vector<std::vector<std::uint8_t>> channels(6);
    for (std::size_t c = 0; c < 6; ++c) {
        channels[c] = {static_cast<std::uint8_t>(c * 2), static_cast<std::uint8_t>(c * 2 + 1)};
    }
    const auto dop = pack(channels);
    for (std::size_t c = 0; c < 6; ++c) {
        std::vector<std::uint8_t> got(2);
        mp::asio::unpack_channel(dop.data(), 6, c, 1, false, got.data());
        INFO("channel " << c);
        CHECK(got == channels[c]);
    }
}

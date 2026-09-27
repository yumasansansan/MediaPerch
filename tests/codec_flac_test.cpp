// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FLAC codec module decodes the format it reports, and nothing else.
//
// **A frame names its own channel count and sample size**, and libFLAC decodes
// what the frame says without holding it to STREAMINFO. The codec reports
// STREAMINFO's format, and wrote each frame out as that format -- so a frame of
// fewer channels than STREAMINFO had was read past the channels libFLAC gave
// it, into pointers that are null. fuzz/codec_fuzzer.cpp found it in seconds, a
// frame of one channel in a stream said to have six; a file whose header CRCs
// are right says the same, so it was not only a fuzzer's input. These take a
// real frame, from the FLAC seed file the fuzzers start from, and hand it to
// the codec under a STREAMINFO that disagrees with it.

#include "module_loader.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#if defined(MEDIAPERCH_CODEC_FLAC)

namespace {

using mp::test::Module;

/// A FLAC file's STREAMINFO and what follows its metadata: the first frame, and
/// the rest of the file after it, which the codec does not read past the frame.
struct Taken {
    std::array<std::uint8_t, 34> streaminfo{};
    std::vector<std::uint8_t> frames;
};

Taken take_apart(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>{in},
                                         std::istreambuf_iterator<char>{}};
    Taken taken;
    REQUIRE(file.size() > 42);
    REQUIRE(file[0] == 'f');
    REQUIRE(file[1] == 'L');
    REQUIRE(file[2] == 'a');
    REQUIRE(file[3] == 'C');
    std::size_t at = 4;
    for (bool last = false; !last;) {
        REQUIRE(at + 4 <= file.size());
        last = (file[at] & 0x80u) != 0;
        const unsigned kind = file[at] & 0x7Fu;
        const std::size_t length = std::size_t{file[at + 1]} << 16 |
                                   std::size_t{file[at + 2]} << 8 | file[at + 3];
        REQUIRE(at + 4 + length <= file.size());
        if (kind == 0) {
            REQUIRE(length == taken.streaminfo.size());
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(at + 4), length,
                        taken.streaminfo.begin());
        }
        at += 4 + length;
    }
    taken.frames.assign(file.begin() + static_cast<std::ptrdiff_t>(at), file.end());
    return taken;
}

std::uint32_t channels_of(const std::array<std::uint8_t, 34>& si)
{
    return ((si[12] >> 1) & 0x07u) + 1;
}

std::uint32_t bits_of(const std::array<std::uint8_t, 34>& si)
{
    return (((si[12] & 0x01u) << 4) | (si[13] >> 4)) + 1;
}

void set_channels(std::array<std::uint8_t, 34>& si, std::uint32_t channels)
{
    si[12] = static_cast<std::uint8_t>((si[12] & 0xF1u) | ((channels - 1) << 1));
}

void set_bits(std::array<std::uint8_t, 34>& si, std::uint32_t bits)
{
    si[12] = static_cast<std::uint8_t>((si[12] & 0xFEu) | ((bits - 1) >> 4));
    si[13] = static_cast<std::uint8_t>((si[13] & 0x0Fu) | (((bits - 1) & 0x0Fu) << 4));
}

struct Decoded {
    MpResult result = MP_ERR_INTERNAL;
    std::size_t bytes = 0;
};

/// Opens the codec on `streaminfo` and decodes the first frame of `frames`.
Decoded decode_first(const MpCodecVtbl& v, const std::array<std::uint8_t, 34>& streaminfo,
                     const std::vector<std::uint8_t>& frames)
{
    MpCodecInstance* c = nullptr;
    REQUIRE(v.open(MP_CODEC_FLAC, streaminfo.data(),
                   static_cast<std::uint32_t>(streaminfo.size()), &c) == MP_OK);
    REQUIRE(c != nullptr);
    std::vector<std::uint8_t> out(std::size_t{1} << 20);
    Decoded decoded;
    decoded.result = v.decode(c, frames.data(), frames.size(), out.data(), out.size(),
                              &decoded.bytes);
    v.close(c);
    return decoded;
}

const std::filesystem::path k_seed =
    std::filesystem::path{MEDIAPERCH_SOURCE_DIR} / "fuzz" / "corpus" / "flac" / "seed16.flac";

} // namespace

TEST_CASE("a FLAC frame decodes to the format its STREAMINFO says", "[codec][flac]")
{
    const Module module{MEDIAPERCH_CODEC_FLAC, MP_KIND_CODEC};
    const auto* v = module.as<MpCodecVtbl>();
    REQUIRE(v != nullptr);
    const Taken taken = take_apart(k_seed);
    REQUIRE(channels_of(taken.streaminfo) == 2);
    REQUIRE(bits_of(taken.streaminfo) == 16);

    const Decoded decoded = decode_first(*v, taken.streaminfo, taken.frames);
    CHECK(decoded.result == MP_OK);
    CHECK(decoded.bytes > 0);
    CHECK(decoded.bytes % 4 == 0); // whole frames of two 16-bit channels
}

TEST_CASE("a FLAC frame of fewer channels than STREAMINFO's is refused, not read past",
          "[codec][flac]")
{
    const Module module{MEDIAPERCH_CODEC_FLAC, MP_KIND_CODEC};
    const auto* v = module.as<MpCodecVtbl>();
    REQUIRE(v != nullptr);
    Taken taken = take_apart(k_seed);
    set_channels(taken.streaminfo, 6);

    const Decoded decoded = decode_first(*v, taken.streaminfo, taken.frames);
    CHECK(decoded.result == MP_ERR_FORMAT);
    CHECK(decoded.bytes == 0);
}

TEST_CASE("a FLAC frame of more channels than STREAMINFO's is refused, not cut short",
          "[codec][flac]")
{
    const Module module{MEDIAPERCH_CODEC_FLAC, MP_KIND_CODEC};
    const auto* v = module.as<MpCodecVtbl>();
    REQUIRE(v != nullptr);
    Taken taken = take_apart(k_seed);
    set_channels(taken.streaminfo, 1);

    const Decoded decoded = decode_first(*v, taken.streaminfo, taken.frames);
    CHECK(decoded.result == MP_ERR_FORMAT);
    CHECK(decoded.bytes == 0);
}

TEST_CASE("a FLAC frame of another sample size than STREAMINFO's is refused",
          "[codec][flac]")
{
    const Module module{MEDIAPERCH_CODEC_FLAC, MP_KIND_CODEC};
    const auto* v = module.as<MpCodecVtbl>();
    REQUIRE(v != nullptr);
    Taken taken = take_apart(k_seed);
    set_bits(taken.streaminfo, 24);
    REQUIRE(bits_of(taken.streaminfo) == 24);

    const Decoded decoded = decode_first(*v, taken.streaminfo, taken.frames);
    CHECK(decoded.result == MP_ERR_FORMAT);
    CHECK(decoded.bytes == 0);
}

#endif

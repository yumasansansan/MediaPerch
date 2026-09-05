// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two AV1 decoders, byte for byte.
//
// **This is the strongest check this tree can make on a video decoder**, and it
// is the reason libaom is a submodule at all. AV1's decoding process is defined
// bit-exactly, so a conformant decoder has exactly one right answer for every
// sample of every frame -- which means dav1d and libaom must agree completely
// or one of them is wrong.
//
// Everything else a video test can do is weaker by a wide margin.
// `codec_dav1d_test.cpp` checks that a frame has a spread of luminance and more
// than one hue, which rules out a cleared buffer, a stuck decoder and a dropped
// chroma plane -- and would pass just as happily on a picture that was subtly,
// consistently wrong. Two independent implementations agreeing on 442 kilobytes
// would not.
//
// It is the method §12 already uses for audio, arriving for video: one decoder
// against another. The half that is still missing is both against what was
// encoded, which for a lossy codec is `compare` rather than a hash.
//
// libaom is slow, and that does not matter here: twenty-four frames of 128x96
// is a conformance check, not a playback measurement.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

struct Module {
    Module(const char* path, MpKind kind)
    {
        auto* dll = ::LoadLibraryA(path);
        if (dll == nullptr) {
            return;
        }
        library = dll;
        using Entry = const MpModuleDesc*(MP_CALL*)(std::uint32_t);
        auto* entry = reinterpret_cast<Entry>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "mp_module_entry")));
        if (entry == nullptr) {
            return;
        }
        const MpModuleDesc* found = entry(MP_ABI_VERSION);
        if (found == nullptr || found->kind != kind) {
            return;
        }
        desc = found;
        vtbl = found->vtbl;
    }
    ~Module()
    {
        if (library != nullptr) {
            if (desc != nullptr && desc->shutdown != nullptr) {
                desc->shutdown();
            }
            ::FreeLibrary(static_cast<HMODULE>(library));
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* library = nullptr;
    const MpModuleDesc* desc = nullptr;
    const void* vtbl = nullptr;
};

/// One decoded frame, copied out.
///
/// **Copied because the ABI says so**: a frame is valid until the next call on
/// the same codec, since a decoder is handing out a slice of a pool it owns.
/// Comparing two decoders means holding both, so both are copied -- and the
/// copy is tight, which also normalises the two libraries' different strides
/// away. A stride is an allocation detail; a sample is not.
struct Frame {
    MpPixelLayout layout{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pts = 0;
    std::vector<std::uint8_t> planes[3];
};

Frame copy_of(const MpVideoFrame& in)
{
    Frame out;
    out.layout = in.layout;
    out.width = in.width;
    out.height = in.height;
    out.pts = in.pts;

    const std::uint32_t count = mp_pixel_planes(&in.layout);
    const std::uint32_t sample = mp_pixel_component_bytes(&in.layout);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t w =
            i == 0 ? in.width : mp_pixel_chroma_width(&in.layout, in.width);
        const std::uint32_t h =
            i == 0 ? in.height : mp_pixel_chroma_height(&in.layout, in.height);
        const std::uint32_t row = w * sample;
        out.planes[i].resize(static_cast<std::size_t>(row) * h);
        const auto* src = static_cast<const std::uint8_t*>(in.plane[i]);
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(out.planes[i].data() + static_cast<std::size_t>(y) * row,
                        src + static_cast<std::size_t>(y) * in.stride[i], row);
        }
    }
    return out;
}

/// Every frame of the fixture, through one decoder.
std::vector<Frame> decode_all(const MpVideoCodecVtbl& codec, const MpDemuxVtbl& demux_vtbl,
                              const char* path)
{
    std::vector<Frame> frames;
    mp::Demux demux;
    if (demux.open(demux_vtbl, path) != MP_OK) {
        return frames;
    }
    std::uint32_t stream = 0;
    MpStreamInfo info{};
    bool found = false;
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        if (demux.stream_info(i, info) && info.kind == MP_STREAM_VIDEO) {
            stream = i;
            found = true;
            break;
        }
    }
    if (!found || info.codec != MP_CODEC_AV1) {
        return frames;
    }

    std::vector<std::uint8_t> config;
    demux.stream_config(stream, config);
    MpVideoCodec* decoder = nullptr;
    if (codec.open(MP_CODEC_AV1, nullptr, config.data(),
                   static_cast<std::uint32_t>(config.size()), &decoder) != MP_OK) {
        return frames;
    }

    const std::uint32_t only_video[] = {stream};
    demux.select_streams(only_video);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec.next_frame(decoder, &frame);
            if (r != MP_OK) {
                return;
            }
            frames.push_back(copy_of(frame));
        }
    };

    while (demux.read_packet(buffer, packet) == MP_OK) {
        if (codec.decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
            MP_ERR_BUSY) {
            drain();
            codec.decode(decoder, buffer.data(), packet.bytes, packet.frame);
        }
        drain();
    }
    codec.flush(decoder);
    drain();

    codec.close(decoder);
    return frames;
}

} // namespace

TEST_CASE("dav1d and the reference decoder agree on every sample",
          "[video][av1][cross]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module dav1d_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    Module aom_module{MEDIAPERCH_CODEC_AOM, MP_KIND_VCODEC};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(dav1d_module.vtbl != nullptr);
    REQUIRE(aom_module.vtbl != nullptr);

    const auto& demux_vtbl = *static_cast<const MpDemuxVtbl*>(demux_module.vtbl);
    const std::vector<Frame> fast = decode_all(
        *static_cast<const MpVideoCodecVtbl*>(dav1d_module.vtbl), demux_vtbl,
        MEDIAPERCH_TEST_AV1);
    const std::vector<Frame> reference = decode_all(
        *static_cast<const MpVideoCodecVtbl*>(aom_module.vtbl), demux_vtbl,
        MEDIAPERCH_TEST_AV1);

    REQUIRE(fast.size() == 24u);
    REQUIRE(reference.size() == fast.size());

    for (std::size_t f = 0; f < fast.size(); ++f) {
        INFO("frame " << f);
        const Frame& a = fast[f];
        const Frame& b = reference[f];

        // **The layout has to match before the samples can be compared**, and
        // it is worth checking rather than assuming: two decoders that
        // disagreed about the chroma subsampling would still produce planes
        // that could be memcmp'd, and the comparison would be meaningless.
        REQUIRE(a.width == b.width);
        REQUIRE(a.height == b.height);
        REQUIRE(a.layout.chroma == b.layout.chroma);
        REQUIRE(a.layout.bits == b.layout.bits);
        REQUIRE(a.layout.container_bits == b.layout.container_bits);
        REQUIRE(a.layout.shift == b.layout.shift);

        for (std::uint32_t p = 0; p < mp_pixel_planes(&a.layout); ++p) {
            INFO("plane " << p);
            REQUIRE(a.planes[p].size() == b.planes[p].size());
            REQUIRE(!a.planes[p].empty());
            // One assertion for the whole plane rather than one per sample:
            // Catch2 counts assertions, and a byte-by-byte loop over 442
            // kilobytes would drown every other number in the run.
            const bool same = std::memcmp(a.planes[p].data(), b.planes[p].data(),
                                          a.planes[p].size()) == 0;
            CHECK(same);
        }
    }

    // And the timestamps, which travel a different route through each decoder:
    // dav1d carries them on its `Dav1dData`, libaom on `user_priv`. Getting one
    // of those wrong would show as A/V drift and nothing else.
    for (std::size_t f = 0; f < fast.size(); ++f) {
        INFO("frame " << f);
        CHECK(fast[f].pts == reference[f].pts);
    }
}

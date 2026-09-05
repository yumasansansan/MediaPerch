// SPDX-License-Identifier: GPL-3.0-or-later
//
// AV1 through dav1d, and the first real producer the presenter's planar path
// has ever had.
//
// **Two things meet here for the first time.** ABI v4 let a frame describe its
// pixels instead of naming them, and the presenter grew the shapes that
// description can take -- but every planar frame it had seen until now was
// built by hand in `video_d3d11_test.cpp`, which proves the arithmetic and not
// the join. dav1d is a decoder that actually produces one, with its significant
// bits at the bottom of the container where P010 puts them at the top. If
// `shift` were wrong in either direction, this is the test that says so.
//
// The chain is the same one §9.8 argues for and `codec_mft_test.cpp` already
// walks for H.264: demux_mp4 reads the container, the codec decodes the
// bitstream, video_d3d11 converts it by the matrix the container named, and
// read_back hands over single-precision linear light. Nothing is looked at.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/// A module, loaded the way the engine loads one.
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
            ::FreeLibrary(static_cast<HMODULE>(library));
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* library = nullptr;
    const MpModuleDesc* desc = nullptr;
    const void* vtbl = nullptr;
};

} // namespace

TEST_CASE("dav1d claims AV1 and declines what it does not decode",
          "[video][av1][dav1d]")
{
    Module module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    REQUIRE(module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(module.vtbl);

    // Declared rather than asked, which is what lets a registry build its
    // resolution table without loading everything.
    REQUIRE(module.desc->codecs != nullptr);
    REQUIRE(module.desc->codec_count == 1u);
    CHECK(module.desc->codecs[0] == MP_CODEC_AV1);

    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_AV1, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
    CHECK(score == 100u);
    // **Still claimed when a presenter has a device**, below what a decoder
    // that landed in a texture on that device would score. Refusing would leave
    // a host with a D3D11 presenter unable to decode AV1 at all, since nothing
    // here decodes it on a GPU.
    REQUIRE(codec->probe(MP_CODEC_AV1, MP_GRAPHICS_D3D11, nullptr, 0, &score) == MP_OK);
    CHECK(score == 60u);
    REQUIRE(codec->probe(MP_CODEC_AV1, MP_GRAPHICS_D3D12, nullptr, 0, &score) == MP_OK);
    CHECK(score == 60u);

    for (MpCodec other : {MP_CODEC_H264, MP_CODEC_HEVC, MP_CODEC_FLAC}) {
        REQUIRE(codec->probe(other, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
        CHECK(score == 0u);
    }

    // An av1C whose first byte is not the marker and version is a record this
    // module declines rather than guesses at.
    const std::uint8_t wrong[] = {0x00u, 0x00u, 0x00u, 0x00u};
    REQUIRE(codec->probe(MP_CODEC_AV1, MP_GRAPHICS_NONE, wrong, sizeof(wrong), &score) ==
            MP_OK);
    CHECK(score == 0u);
}

TEST_CASE("AV1 decodes to planar frames with the bits at the bottom",
          "[video][av1][dav1d]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV1) == MP_OK);
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
    REQUIRE(found);
    REQUIRE(info.codec == MP_CODEC_AV1);

    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_AV1, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    // **The sequence header is read at open, not after the first frame.** An
    // `av1C` carries it, so the geometry is answerable before anything is
    // decoded -- which a host sizing a window needs and a hardware decoder that
    // learns from the bitstream cannot give.
    MpVideoInfo early{};
    early.size = sizeof(early);
    REQUIRE(codec->get_format(decoder, &early) == MP_OK);
    CHECK(early.width == 128u);
    CHECK(early.height == 96u);
    // **Zero, and here it means unchanged rather than untimed.** dav1d hands
    // back the timestamp it was given, so the demuxer's timescale still holds.
    CHECK(early.timescale == 0u);

    const std::uint32_t only_video[] = {stream};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t frames = 0;
    bool any_content = false;
    MpPixelLayout seen{};

    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec->next_frame(decoder, &frame);
            if (r == MP_END) {
                return;
            }
            REQUIRE(r == MP_OK);
            ++frames;
            CHECK(frame.width == 128u);
            CHECK(frame.height == 96u);
            // dav1d is CPU: planes, never a texture.
            REQUIRE(frame.texture == nullptr);
            seen = frame.layout;

            REQUIRE(mp_pixel_planes(&frame.layout) == 3u);
            for (std::uint32_t i = 0; i < 3u; ++i) {
                REQUIRE(frame.plane[i] != nullptr);
                CHECK(frame.stride[i] > 0u);
            }
            // testsrc2 is bars and shapes, so the luma plane is not one value
            // repeated -- the cheapest evidence that something was decoded
            // rather than a cleared buffer handed back.
            const auto* luma = static_cast<const std::uint8_t*>(frame.plane[0]);
            for (std::uint32_t x = 1; x < frame.width; ++x) {
                if (luma[x] != luma[0]) {
                    any_content = true;
                    break;
                }
            }
        }
    };

    while (demux.read_packet(buffer, packet) == MP_OK) {
        if (codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
            MP_ERR_BUSY) {
            drain();
            REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
                    MP_OK);
        }
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();

    CHECK(frames == 24u);
    CHECK(any_content);

    // **The layout, and the field this whole ABI version exists for.** 4:2:0
    // planar, eight bits in eight, and a shift of zero -- where the same
    // decoder at ten bits would put ten in sixteen with a shift of zero and
    // the hardware decoder's P010 puts ten in sixteen with a shift of six.
    CHECK(seen.chroma == MP_CHROMA_420);
    CHECK(seen.packing == MP_PACK_PLANAR);
    CHECK(seen.bits == 8u);
    CHECK(seen.container_bits == 8u);
    CHECK(seen.shift == 0u);
    CHECK(mp_pixel_sample_scale(&seen) == Catch::Approx(1.0).epsilon(0));

    // And a reset puts the sequence header back in front, so the decoder is
    // usable after a seek rather than silently empty.
    REQUIRE(codec->reset(decoder) == MP_OK);
    codec->close(decoder);
}

TEST_CASE("a decoded AV1 frame reaches the presenter and comes back as pixels",
          "[video][av1][dav1d][d3d11]")
{
    // The whole chain, on the planar path this time. `codec_mft_test.cpp` walks
    // the same one for H.264, which is semi-planar -- so between them the two
    // arrangements the presenter supports both have a producer.
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    REQUIRE(video_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);
    const auto* video = static_cast<const MpVideoVtbl*>(video_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV1) == MP_OK);
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
    REQUIRE(found);

    // **The container's colour, not the decoder's**, which is §9.8's join and
    // the same way round it is for H.264.
    MpVideoInfo geometry{};
    geometry.size = sizeof(geometry);
    REQUIRE(demux.video_info(stream, geometry));

    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_AV1, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    MpVideo* presenter = nullptr;
    REQUIRE(video->open(nullptr, &presenter) == MP_OK);
    REQUIRE(video->set(presenter, "device", "warp") == MP_OK);
    REQUIRE(video->configure(presenter, &geometry) == MP_OK);

    const std::uint32_t only_video[] = {stream};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t presented = 0;

    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec->next_frame(decoder, &frame);
            if (r == MP_END) {
                return;
            }
            REQUIRE(r == MP_OK);
            REQUIRE(video->present(presenter, &frame) == MP_OK);
            ++presented;
        }
    };

    while (demux.read_packet(buffer, packet) == MP_OK) {
        if (codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
            MP_ERR_BUSY) {
            drain();
            REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
                    MP_OK);
        }
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();
    CHECK(presented == 24u);

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    MpPixelLayout layout{};
    layout.size = sizeof(layout);
    REQUIRE(video->read_back(presenter, nullptr, 0, &width, &height, &layout) ==
            MP_ERR_NO_MEMORY);
    CHECK(width == 128u);
    CHECK(height == 96u);
    REQUIRE(layout.container_bits == 32u);

    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4u);
    REQUIRE(video->read_back(presenter, pixels.data(), pixels.size() * sizeof(float),
                             &width, &height, &layout) == MP_OK);

    // testsrc2 is bars and shapes: a spread of luminance and more than one hue,
    // neither of which a cleared buffer or a dropped chroma plane would give.
    float darkest = 2.0f;
    float brightest = -1.0f;
    bool coloured = false;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const float r = pixels[i];
        const float g = pixels[i + 1];
        const float b = pixels[i + 2];
        const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        darkest = std::min(darkest, luminance);
        brightest = std::max(brightest, luminance);
        if (std::abs(r - b) > 0.05f || std::abs(g - b) > 0.05f) {
            coloured = true;
        }
        // Linear light out of an SDR frame stays inside the range its transfer
        // is defined over.
        CHECK(r >= -0.001f);
        CHECK(r <= 1.001f);
    }
    CHECK(darkest < 0.2f);
    CHECK(brightest > 0.5f);
    CHECK(coloured);

    video->close(presenter);
    codec->close(decoder);
}

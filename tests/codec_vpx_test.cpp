// SPDX-License-Identifier: GPL-3.0-or-later
//
// VP8 and VP9, by the reference implementation, out of a WebM.
//
// **Two things meet here that had never met.** `demux_mkv` named no video codec
// at all until this work -- every video track in a Matroska came back
// MP_CODEC_UNKNOWN, so the container this tree could split best was the one it
// could decode least. And libvpx is the reference for both codecs, which is
// §7's argument for libFLAC and the Xiph decoders arriving for video.
//
// The chain is the one §9.8 argues for: demux_mkv reads the container,
// codec_vpx decodes the bitstream, video_d3d11 converts it by the matrix the
// container named, and read_back hands over single-precision linear light.
//
// VP8 is here on its own account. Reading only the newer of the two would leave
// half the reason libvpx is a dependency, and a VP8 WebM is a file people
// still have.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

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

} // namespace

TEST_CASE("libvpx claims VP8 and VP9 and declines the rest", "[video][vpx]")
{
    Module module{MEDIAPERCH_CODEC_VPX, MP_KIND_VCODEC};
    REQUIRE(module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(module.vtbl);

    REQUIRE(module.desc->codecs != nullptr);
    REQUIRE(module.desc->codec_count == 2u);

    std::uint32_t score = 0;
    for (MpCodec c : {MP_CODEC_VP8, MP_CODEC_VP9}) {
        REQUIRE(codec->probe(c, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
        // **100, unlike libaom's 40.** libaom scores low because dav1d exists
        // and is what a player should use; nothing else here reads VP8 or VP9
        // at all, so the reference is also the answer.
        CHECK(score == 100u);
    }
    for (MpCodec other : {MP_CODEC_AV1, MP_CODEC_H264, MP_CODEC_FLAC}) {
        REQUIRE(codec->probe(other, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
        CHECK(score == 0u);
    }
}

TEST_CASE("VP8 and VP9 decode out of a WebM and reach the presenter",
          "[video][vpx][d3d11]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MKV, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_VPX, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    REQUIRE(video_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);
    const auto* video = static_cast<const MpVideoVtbl*>(video_module.vtbl);

    struct Case {
        const char* path;
        MpCodec codec;
        const char* what;
    };
    const Case cases[] = {
        {MEDIAPERCH_TEST_VP9, MP_CODEC_VP9, "VP9"},
        {MEDIAPERCH_TEST_VP8, MP_CODEC_VP8, "VP8"},
    };

    for (const Case& one : cases) {
        INFO(one.what);
        mp::Demux demux;
        REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                           one.path) == MP_OK);

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
        // **The line demux_mkv did not have.** Every video track in a Matroska
        // was MP_CODEC_UNKNOWN before this, so the container split perfectly
        // and nothing could decode what came out.
        REQUIRE(info.codec == one.codec);

        // VP8 and VP9 carry no CodecPrivate in Matroska: the bitstream says
        // everything and libvpx needs no configuration record.
        std::vector<std::uint8_t> config;
        (void)demux.stream_config(stream, config);
        CHECK(config.empty());

        MpVideoInfo geometry{};
        geometry.size = sizeof(geometry);
        REQUIRE(demux.video_info(stream, geometry));

        MpVideoCodec* decoder = nullptr;
        REQUIRE(codec->open(one.codec, nullptr, config.data(),
                            static_cast<std::uint32_t>(config.size()),
                            &decoder) == MP_OK);

        MpVideo* presenter = nullptr;
        REQUIRE(video->open(nullptr, &presenter) == MP_OK);
        REQUIRE(video->set(presenter, "device", "warp") == MP_OK);
        REQUIRE(video->configure(presenter, &geometry) == MP_OK);

        const std::uint32_t only_video[] = {stream};
        REQUIRE(demux.select_streams(only_video) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint32_t frames = 0;
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
                seen = frame.layout;
                REQUIRE(frame.texture == nullptr); // libvpx is CPU
                REQUIRE(mp_pixel_planes(&frame.layout) == 3u);
                REQUIRE(video->present(presenter, &frame) == MP_OK);
            }
        };

        while (demux.read_packet(buffer, packet) == MP_OK) {
            REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes,
                                  packet.frame) == MP_OK);
            drain();
        }
        REQUIRE(codec->flush(decoder) == MP_OK);
        drain();

        CHECK(frames == 24u);
        // 4:2:0 planar at eight bits, with the significant bits at the bottom
        // of their container -- the same shape dav1d and libaom produce, and
        // the opposite of the hardware decoder's P010.
        CHECK(seen.chroma == MP_CHROMA_420);
        CHECK(seen.packing == MP_PACK_PLANAR);
        CHECK(seen.bits == 8u);
        CHECK(seen.container_bits == 8u);
        CHECK(seen.shift == 0u);

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        MpPixelLayout layout{};
        layout.size = sizeof(layout);
        REQUIRE(video->read_back(presenter, nullptr, 0, &width, &height, &layout) ==
                MP_ERR_NO_MEMORY);
        CHECK(width == 128u);
        CHECK(height == 96u);

        std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4u);
        REQUIRE(video->read_back(presenter, pixels.data(),
                                 pixels.size() * sizeof(float), &width, &height,
                                 &layout) == MP_OK);

        // testsrc2 is bars and shapes: a spread of luminance and more than one
        // hue, neither of which a cleared buffer or a dropped chroma plane
        // would give.
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
        }
        CHECK(darkest < 0.2f);
        CHECK(brightest > 0.5f);
        CHECK(coloured);

        video->close(presenter);
        codec->close(decoder);
    }
}

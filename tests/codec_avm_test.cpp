// SPDX-License-Identifier: GPL-3.0-or-later
//
// AV2, through the only decoder it has.
//
// **There is no cross-check here and that is the point of saying so.**
// `av1_cross_test.cpp` holds dav1d and libaom to byte equality, which is the
// strongest statement this tree makes about a video decoder. AV2 cannot have
// that yet: avm is the only implementation, and dav2d -- a submodule here
// already -- has no module. So this test is the weaker kind, the same one
// `codec_dav1d_test.cpp` and `codec_vpx_test.cpp` make on their own: the
// container names the codec, the decoder produces frames of the shape the ABI
// describes, and a picture reaches the presenter with a spread of luminance and
// more than one hue in it. That rules out a cleared buffer and a dropped chroma
// plane, and not much more.
//
// It also pins two things that were read out of avm's source rather than a
// specification, and would otherwise be beliefs: that a Matroska AV2 track is
// spelled `V_AV2`, and that its CodecPrivate is a four-byte Av2Config.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

/// What `tools/make_av_fixture.py` encodes: sixteen frames rather than the
/// twenty-four the other video fixtures carry, because the reference encoder is
/// minutes where libvpx is seconds.
constexpr std::uint32_t k_frames = 16;

} // namespace

TEST_CASE("avm claims AV2 and declines everything else", "[video][avm]")
{
    Module module{MEDIAPERCH_CODEC_AVM, MP_KIND_VCODEC};
    REQUIRE(module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(module.vtbl);

    REQUIRE(module.desc->codec_count == 1u);
    REQUIRE(module.desc->codecs[0] == MP_CODEC_AV2);

    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_AV2, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
    // **40, though nothing else decodes AV2 today.** A score decides between
    // rivals; when dav2d arrives it should outrank this without an edit here.
    CHECK(score == 40u);

    // AV1 is the near miss worth naming: one codec number away, one fork away,
    // and a module that claimed it would be picked over both dav1d and libaom's
    // careful 40.
    for (MpCodec other : {MP_CODEC_AV1, MP_CODEC_VP9, MP_CODEC_H264}) {
        REQUIRE(codec->probe(other, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
        CHECK(score == 0u);
    }

    // A record that is present has exactly one legal shape. Four bytes with the
    // marker is an Av2Config; anything else means the container and this module
    // disagree about what AV2 is.
    const std::uint8_t good[4] = {0x81u, 0x00u, 0x00u, 0x00u};
    const std::uint8_t no_marker[4] = {0x01u, 0x00u, 0x00u, 0x00u};
    const std::uint8_t too_long[5] = {0x81u, 0x00u, 0x00u, 0x00u, 0x00u};
    REQUIRE(codec->probe(MP_CODEC_AV2, MP_GRAPHICS_NONE, good, 4, &score) == MP_OK);
    CHECK(score == 40u);
    REQUIRE(codec->probe(MP_CODEC_AV2, MP_GRAPHICS_NONE, no_marker, 4, &score) == MP_OK);
    CHECK(score == 0u);
    REQUIRE(codec->probe(MP_CODEC_AV2, MP_GRAPHICS_NONE, too_long, 5, &score) == MP_OK);
    CHECK(score == 0u);

    MpVideoCodec* decoder = nullptr;
    CHECK(codec->open(MP_CODEC_AV2, nullptr, no_marker, 4, &decoder) == MP_ERR_FORMAT);
    CHECK(codec->open(MP_CODEC_AV1, nullptr, nullptr, 0, &decoder) == MP_ERR_INVALID);
}

TEST_CASE("AV2 decodes out of a WebM and reaches the presenter", "[video][avm][d3d11]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MKV, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_AVM, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    REQUIRE(video_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);
    const auto* video = static_cast<const MpVideoVtbl*>(video_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV2) == MP_OK);

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
    // V_AV2 is not in Matroska's codec registry. It is what avm's own muxer
    // writes, which is what makes it the spelling to read.
    REQUIRE(info.codec == MP_CODEC_AV2);

    // The Av2Config record: four bytes, marker first, and no configuration OBUs
    // in it -- which is why `codec_open` has nothing to replay.
    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    REQUIRE(config.size() == 4u);
    CHECK(config[0] == 0x81u);

    MpVideoInfo geometry{};
    geometry.size = sizeof(geometry);
    REQUIRE(demux.video_info(stream, geometry));
    CHECK(geometry.width == 128u);
    CHECK(geometry.height == 96u);

    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_AV2, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    // Before the first frame there is no format to give: avm, like libaom and
    // unlike dav1d, has no public way to parse a sequence header without
    // decoding one.
    MpVideoInfo early{};
    early.size = sizeof(early);
    CHECK(codec->get_format(decoder, &early) == MP_ERR_BUSY);

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
            REQUIRE(frame.texture == nullptr); // avm is CPU
            REQUIRE(mp_pixel_planes(&frame.layout) == 3u);
            REQUIRE(video->present(presenter, &frame) == MP_OK);
        }
    };

    while (demux.read_packet(buffer, packet) == MP_OK) {
        REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
                MP_OK);
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();

    CHECK(frames == k_frames);

    MpVideoInfo late{};
    late.size = sizeof(late);
    REQUIRE(codec->get_format(decoder, &late) == MP_OK);
    CHECK(late.width == 128u);
    CHECK(late.height == 96u);
    // Zero means unchanged rather than untimed: the demuxer's timescale still
    // holds, which is what §9.9 settled.
    CHECK(late.timescale == 0u);

    // **Eight bits in a sixteen-bit container, and that is the interesting
    // part.** dav1d, libaom and libvpx all hand back an eight-bit stream in
    // eight-bit planes; avm hands back sixteen, because its decoder has no
    // `allow_lowbitdepth` -- libaom's structure has that field and avm's does
    // not, so AV2 always takes the high bit depth path.
    //
    // That is a shape ABI v3 could not have described. `bool ten_bit` had one
    // question and two answers, and eight-in-sixteen is neither of them; v4
    // states it as three numbers and the presenter scales by the ratio between
    // them. The picture checks below are what say it arrived right rather than
    // sixty-four times too dark.
    CHECK(seen.chroma == MP_CHROMA_420);
    CHECK(seen.packing == MP_PACK_PLANAR);
    CHECK(seen.bits == 8u);
    CHECK(seen.container_bits == 16u);
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
    REQUIRE(video->read_back(presenter, pixels.data(), pixels.size() * sizeof(float),
                             &width, &height, &layout) == MP_OK);

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

    // A reset has to leave a decoder that still works, and avm's has no
    // configuration record to replay afterwards -- the sequence header is in
    // the stream, on the keyframe a seek lands on.
    REQUIRE(codec->reset(decoder) == MP_OK);
    REQUIRE(demux.seek(stream, 0) == MP_OK);
    frames = 0;
    while (demux.read_packet(buffer, packet) == MP_OK) {
        REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
                MP_OK);
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();
    CHECK(frames == k_frames);

    video->close(presenter);
    codec->close(decoder);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// HEVC through libde265, which is the first HEVC decoder this tree owns.
//
// **What it replaces is a hope.** `codec_mft` asks Media Foundation, and
// measured on this machine the only transform that answers for HEVC is
// `HEVCVideoExtension` -- a Store package, absent from a clean Windows, and one
// that turned out to offer no NV12 or P010 output when it was finally asked.
// So until now this tree could open an HEVC file on exactly the machines where
// somebody had already installed something, and could not say which those were.
//
// The chain is `codec_dav1d_test.cpp`'s: demux_mp4 reads the container, the
// codec decodes the bitstream, and what comes back is planes rather than
// anything anybody looks at.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include "module_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using mp::test::Module;

namespace {

/// The video stream of `path`, opened, with its `hvcC` in `config`.
struct Opened {
    mp::Demux demux;
    std::uint32_t stream = 0;
    MpStreamInfo info{};
    std::vector<std::uint8_t> config;
};

void open_video(const MpDemuxVtbl& vtbl, const char* path, Opened& out)
{
    REQUIRE(out.demux.open(vtbl, path) == MP_OK);
    bool found = false;
    for (std::uint32_t i = 0; i < out.demux.stream_count(); ++i) {
        if (out.demux.stream_info(i, out.info) && out.info.kind == MP_STREAM_VIDEO) {
            out.stream = i;
            found = true;
            break;
        }
    }
    REQUIRE(found);
    REQUIRE(out.demux.stream_config(out.stream, out.config));
    const std::uint32_t only_video[] = {out.stream};
    REQUIRE(out.demux.select_streams(only_video) == MP_OK);
}

} // namespace

TEST_CASE("libde265 claims HEVC and declines what it does not decode",
          "[video][hevc][de265]")
{
    Module module{MEDIAPERCH_CODEC_DE265, MP_KIND_VCODEC};
    REQUIRE(module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(module.vtbl);
    REQUIRE(codec->probe != nullptr);

    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Opened file;
    open_video(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl), MEDIAPERCH_TEST_HEVC,
               file);
    REQUIRE(file.info.codec == MP_CODEC_HEVC);
    const auto config_bytes = static_cast<std::uint32_t>(file.config.size());

    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_HEVC, MP_GRAPHICS_NONE, file.config.data(), config_bytes,
                         &score) == MP_OK);
    CHECK(score > 0u);

    // **It claims the device case too, at a lower score.** The frames are
    // planes in system memory either way and the host uploads them; what the
    // number says is that a decoder landing in a texture on that very device
    // would be better, not that this one cannot be used. dav1d scores the same
    // shape for the same reason.
    std::uint32_t with_device = 0;
    REQUIRE(codec->probe(MP_CODEC_HEVC, MP_GRAPHICS_D3D11, file.config.data(),
                         config_bytes, &with_device) == MP_OK);
    CHECK(with_device > 0u);
    CHECK(with_device < score);

    // Another codec is not this module's.
    std::uint32_t other = 1;
    codec->probe(MP_CODEC_AV1, MP_GRAPHICS_NONE, file.config.data(), config_bytes, &other);
    CHECK(other == 0u);

    // **An HEVC stream with no `hvcC` is declined rather than attempted.** The
    // parameter sets could arrive in band and libde265 would find them -- but
    // then nothing could have declined a ten-bit file first, and §7 says a
    // decoder must not discover mid-file that it cannot do this.
    std::uint32_t bare = 1;
    codec->probe(MP_CODEC_HEVC, MP_GRAPHICS_NONE, nullptr, 0, &bare);
    CHECK(bare == 0u);

    // And a record that is not one.
    const std::uint8_t nonsense[] = {0x00, 0x01, 0x02};
    std::uint32_t rubbish = 1;
    codec->probe(MP_CODEC_HEVC, MP_GRAPHICS_NONE, nonsense, sizeof(nonsense), &rubbish);
    CHECK(rubbish == 0u);
}

TEST_CASE("HEVC decodes to planar frames, every one of them",
          "[video][hevc][de265]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DE265, MP_KIND_VCODEC};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);

    Opened file;
    open_video(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl), MEDIAPERCH_TEST_HEVC,
               file);

    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_HEVC, nullptr, file.config.data(),
                        static_cast<std::uint32_t>(file.config.size()), &decoder) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t frames = 0;
    std::uint32_t packets = 0;
    bool any_content = false;
    MpPixelLayout seen{};
    std::vector<std::uint64_t> stamps;

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
            stamps.push_back(frame.pts);
            CHECK(frame.width == 128u);
            CHECK(frame.height == 96u);
            REQUIRE(frame.texture == nullptr);
            seen = frame.layout;

            REQUIRE(mp_pixel_planes(&frame.layout) == 3u);
            for (std::uint32_t i = 0; i < 3u; ++i) {
                REQUIRE(frame.plane[i] != nullptr);
                CHECK(frame.stride[i] > 0u);
            }
            const auto* luma = static_cast<const std::uint8_t*>(frame.plane[0]);
            for (std::uint32_t x = 1; x < frame.width; ++x) {
                if (luma[x] != luma[0]) {
                    any_content = true;
                    break;
                }
            }
        }
    };

    while (file.demux.read_packet(buffer, packet) == MP_OK) {
        ++packets;
        REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) == MP_OK);
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();

    // **Every frame, and the last one is the hard one.** `de265_decode` says
    // WAITING_FOR_INPUT_DATA when it wants more bytes, and after the flush
    // there are none -- while the final picture sits in the reorder buffer.
    // Treating that code as the end lost exactly one frame per file, silently,
    // with no warning from libde265 because nothing had gone wrong.
    CHECK(packets == 24u);
    CHECK(frames == packets);
    CHECK(any_content);

    // **The timestamps are the pictures', not the packets'.** x265 writes
    // B-frames, so decode order is not display order, and the pts of the last
    // packet pushed is not the pts of the next picture out. Carrying it through
    // libde265 is what keeps them together; attaching the packet's instead was
    // measured as fourteen of twenty-four frames dropped by the pacer.
    REQUIRE(stamps.size() == 24u);
    for (std::size_t i = 1; i < stamps.size(); ++i) {
        INFO("frame " << i << " at " << stamps[i] << " after " << stamps[i - 1]);
        CHECK(stamps[i] > stamps[i - 1]);
    }

    CHECK(seen.chroma == MP_CHROMA_420);
    CHECK(seen.packing == MP_PACK_PLANAR);
    CHECK(seen.bits == 8u);
    CHECK(seen.container_bits == 8u);
    CHECK(seen.shift == 0u);

    // A reset puts the parameter sets back in front, so the decoder is usable
    // after a seek rather than silently empty.
    REQUIRE(codec->reset(decoder) == MP_OK);
    codec->close(decoder);
}

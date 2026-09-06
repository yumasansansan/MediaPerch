// SPDX-License-Identifier: GPL-3.0-or-later
//
// libde265 against HM, sample for sample.
//
// **HEVC's decoding process is defined bit-exactly**, so two conforming
// decoders must agree on every sample of every frame or one of them is wrong.
// That is a far stronger check than anything a single decoder can be held to on
// its own: `codec_de265_test.cpp` can say a frame has more than one luma value,
// which rules out a cleared buffer and very little else. This is the method §12
// already uses for audio -- one decoder against another, and both against what
// was encoded -- and `codec_aom` uses for AV1, arriving for HEVC.
//
// **HM is run as a program**, which is what HM is: `TAppDecTop::decode` reads a
// file, and the library underneath it has no entry point that is not two
// hundred and fifty lines of that app's state. tests/hm/CMakeLists.txt makes
// the argument at length. It also means the reference here is the reference:
// what ITU/ISO/IEC published, driven the way they drive it.
//
// The bitstream HM is given is built by this tree, out of the container, with
// `mp::mft::to_annex_b` -- so the conversion HEVC shares with H.264 is under
// test as well, and by the strongest possible check: if it emitted a NAL wrong,
// HM would decode something else and every sample would differ.

#include "h264.hpp"
#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include "module_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using mp::test::Module;

namespace {

/// Where this test may write. The build directory, which ctest runs in.
std::string scratch(const char* name)
{
    return std::string{"hm_cross_"} + name;
}

/// The whole of a file, or empty.
std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("libde265 and the HEVC reference agree on every sample",
          "[video][hevc][de265][hm]")
{
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DE265, MP_KIND_VCODEC};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_HEVC) == MP_OK);
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
    REQUIRE(info.codec == MP_CODEC_HEVC);

    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    const mp::mft::HevcConfig hvcc =
        mp::mft::parse_hvcc(config.data(), config.size());
    REQUIRE(hvcc.valid);

    const std::uint32_t only_video[] = {stream};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    // ---- the elementary stream, built here out of the container ------------
    //
    // Annex B: the parameter sets, then every sample's NAL units each behind a
    // start code. That is what a decoder reading a file expects and what MP4
    // deliberately does not store.
    const std::string bitstream_path = scratch("stream.265");
    std::vector<std::uint8_t> elementary;
    std::vector<std::uint8_t> sample_annex;
    REQUIRE(mp::mft::parameter_sets_annex_b(hvcc.annex, elementary));

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t samples = 0;
    while (demux.read_packet(buffer, packet) == MP_OK) {
        ++samples;
        REQUIRE(mp::mft::to_annex_b(hvcc.annex, buffer.data(), packet.bytes, false,
                                    sample_annex));
        elementary.insert(elementary.end(), sample_annex.begin(), sample_annex.end());
    }
    REQUIRE(samples == 24u);
    {
        std::ofstream out(bitstream_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(elementary.data()),
                  static_cast<std::streamsize>(elementary.size()));
    }

    // ---- HM, as a program --------------------------------------------------
    const std::string yuv_path = scratch("reference.yuv");
    // **Two pairs of quotes, and the outer one is not decoration.** `system`
    // runs `cmd /c <string>`, and cmd strips the first and last character when
    // the string begins with a quote -- so a command whose program *and*
    // arguments are quoted arrives with its first quote gone and its last one
    // orphaned. Wrapping the whole thing again is what survives that.
    const std::string command = std::string{"\"\""} + MEDIAPERCH_HM_DECODER +
                                "\" -b \"" + bitstream_path + "\" -o \"" + yuv_path +
                                "\" -d 8 > \"" + scratch("hm.log") + "\" 2>&1\"";
    const int ran = std::system(command.c_str());
    INFO("HM said " << ran << "; its output is in " << scratch("hm.log"));
    REQUIRE(ran == 0);

    const std::vector<std::uint8_t> reference = read_file(yuv_path);
    // 4:2:0 at eight bits: one byte a luma sample and half of one per chroma.
    constexpr std::size_t k_width = 128;
    constexpr std::size_t k_height = 96;
    constexpr std::size_t k_frame_bytes = k_width * k_height * 3 / 2;
    INFO("HM wrote " << reference.size() << " bytes");
    REQUIRE(reference.size() == k_frame_bytes * samples);

    // ---- and libde265, through the ABI ------------------------------------
    mp::Demux again;
    REQUIRE(again.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_HEVC) == MP_OK);
    REQUIRE(again.select_streams(only_video) == MP_OK);

    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_HEVC, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    std::uint32_t compared = 0;
    std::uint64_t differing_samples = 0;
    std::size_t first_bad_frame = 0;

    const auto compare = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec->next_frame(decoder, &frame);
            if (r == MP_END) {
                return;
            }
            REQUIRE(r == MP_OK);
            REQUIRE(frame.width == k_width);
            REQUIRE(frame.height == k_height);
            REQUIRE(compared < samples);

            // **HM writes display order, and so does the ABI.** If either
            // reordered differently this would not merely differ, it would
            // differ everywhere -- which is the check working rather than
            // failing.
            const std::uint8_t* want = reference.data() + compared * k_frame_bytes;
            std::uint64_t bad = 0;
            for (std::uint32_t plane = 0; plane < 3; ++plane) {
                const std::size_t w = plane == 0 ? k_width : k_width / 2;
                const std::size_t h = plane == 0 ? k_height : k_height / 2;
                const auto* got = static_cast<const std::uint8_t*>(frame.plane[plane]);
                for (std::size_t y = 0; y < h; ++y) {
                    for (std::size_t x = 0; x < w; ++x) {
                        if (got[y * frame.stride[plane] + x] != want[y * w + x]) {
                            ++bad;
                        }
                    }
                }
                want += w * h;
            }
            if (bad != 0 && differing_samples == 0) {
                first_bad_frame = compared;
            }
            differing_samples += bad;
            ++compared;
        }
    };

    while (again.read_packet(buffer, packet) == MP_OK) {
        REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) == MP_OK);
        compare();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    compare();
    codec->close(decoder);

    CHECK(compared == samples);
    // **Not "close enough".** HEVC is defined bit-exactly and this is the one
    // check in the video path that can say so: a single sample out of
    // 24 * 18432 is a bug in one of the two decoders, not a tolerance.
    INFO("first frame that differed: " << first_bad_frame);
    CHECK(differing_samples == 0u);
}

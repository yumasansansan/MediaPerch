// SPDX-License-Identifier: GPL-3.0-or-later
//
// The video decoder, and the bitstream conversion it needs first.
//
// Two halves, and only one of them needs Windows to have a decoder. `avcC` to
// Annex B is a container question wearing a codec's clothes -- MP4 stores H.264
// as length-prefixed NAL units with the parameter sets out of band, and every
// decoder wants start codes and the parameter sets in the stream -- so it is
// arithmetic over bytes and is tested as such. The decoder itself is driven
// against the H.264 track of the same fixture `demux_v3_test.cpp` uses, through
// `demux_mp4`, which is what makes it an end-to-end check of the split plan.md
// §9.8 argues for rather than of one module in isolation.

#include "h264.hpp"
#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>

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
            // **`shutdown` before `FreeLibrary`, because that is what a host
            // does.** `ModuleRegistry` calls `init` after loading and
            // `shutdown` before unloading; a harness that skipped the second
            // was not modelling the host, it was modelling a host with a bug.
            // `codec_mft` had to stop Media Foundation from a static
            // destructor because nothing here called its shutdown, and doing
            // that during `FreeLibrary` deadlocks against the loader lock.
            if (desc != nullptr && desc->shutdown != nullptr) {
                desc->shutdown();
            }
            ::FreeLibrary(static_cast<HMODULE>(library));
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* library = nullptr;
    /// Kept so the destructor can call `shutdown`, which is what a host does.
    const MpModuleDesc* desc = nullptr;
    const void* vtbl = nullptr;
};

/// A minimal but real `avcC`: version 1, a profile, one SPS and one PPS, and a
/// `lengthSizeMinusOne` of 3 meaning four-byte lengths.
std::vector<std::uint8_t> made_up_avcc(std::uint8_t length_size_minus_one = 3)
{
    return {
        0x01,                          // configurationVersion
        0x64, 0x00, 0x1F,              // profile, compatibility, level
        static_cast<std::uint8_t>(0xFCu | length_size_minus_one),
        0xE1,                          // three reserved bits, then one SPS
        0x00, 0x04, 0x67, 0x64, 0x00, 0x1F, // its length, then four bytes
        0x01,                          // one PPS
        0x00, 0x03, 0x68, 0xEB, 0xE3,  // its length, then three bytes
    };
}

/// An SPS, written bit by bit -- which is how a test gets to state the syntax
/// rather than paste a hex blob nobody can check against the standard.
class SpsWriter {
public:
    SpsWriter()
    {
        // The NAL header: forbidden_zero_bit 0, nal_ref_idc 3, nal_unit_type 7.
        bytes_.push_back(0x67);
    }

    void u(std::uint32_t count, std::uint32_t value)
    {
        for (std::uint32_t i = count; i-- > 0;) {
            put((value >> i) & 1u);
        }
    }

    /// Unsigned Exp-Golomb, the encoder side of what `parse_sps` decodes.
    void ue(std::uint32_t value)
    {
        std::uint32_t bits = 0;
        while ((value + 1u) >> (bits + 1u) != 0) {
            ++bits;
        }
        u(bits, 0);
        u(1, 1);
        u(bits, (value + 1u) - (1u << bits));
    }

    [[nodiscard]] std::vector<std::uint8_t> done()
    {
        // rbsp_trailing_bits: a one, then zeroes to the byte.
        u(1, 1);
        while (bit_ != 0) {
            put(0);
        }
        return bytes_;
    }

private:
    void put(std::uint32_t bit)
    {
        if (bit_ == 0) {
            bytes_.push_back(0);
            bit_ = 8;
        }
        --bit_;
        if (bit != 0) {
            bytes_.back() |= static_cast<std::uint8_t>(1u << bit_);
        }
    }

    std::vector<std::uint8_t> bytes_;
    std::uint32_t bit_ = 0;
};

/// One SPS of the shape the high profiles carry.
std::vector<std::uint8_t> made_up_sps(std::uint32_t profile_idc,
                                      std::uint32_t chroma_format_idc = 1,
                                      std::uint32_t bit_depth_luma_minus8 = 0,
                                      std::uint32_t bit_depth_chroma_minus8 = 0)
{
    SpsWriter w;
    w.u(8, profile_idc);
    w.u(8, 0);  // constraint flags and two reserved bits
    w.u(8, 31); // level_idc
    w.ue(0);    // seq_parameter_set_id
    // Only the profiles that carry these fields write them; the rest infer
    // 4:2:0 and eight bits, which is what the parser must do too.
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244) {
        w.ue(chroma_format_idc);
        if (chroma_format_idc == 3) {
            w.u(1, 0); // separate_colour_plane_flag
        }
        w.ue(bit_depth_luma_minus8);
        w.ue(bit_depth_chroma_minus8);
        w.u(1, 0); // qpprime_y_zero_transform_bypass_flag
        w.u(1, 0); // seq_scaling_matrix_present_flag
    }
    w.ue(4); // log2_max_frame_num_minus4, past what the parser reads
    return w.done();
}

/// An `avcC` carrying one SPS and one PPS.
std::vector<std::uint8_t> avcc_with(const std::vector<std::uint8_t>& sps)
{
    std::vector<std::uint8_t> out{0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE1};
    out.push_back(static_cast<std::uint8_t>(sps.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(sps.size() & 0xFFu));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(0x01); // one PPS
    out.insert(out.end(), {0x00, 0x03, 0x68, 0xEB, 0xE3});
    return out;
}

} // namespace

TEST_CASE("a sequence parameter set says what shape its pixels are",
          "[codec][mft][avcc][sps]")
{
    // **Read so that a stream can be declined**, which is the whole point: every
    // H.264 decoder behind Media Foundation is 8-bit 4:2:0, and the high
    // profiles reach further. No MF, no GPU and no file -- bits in, four fields
    // out.
    using mp::mft::parse_sps;

    SECTION("Baseline and Main infer 4:2:0 at eight bits")
    {
        // The fields are simply absent from the syntax for these profiles, so a
        // parser that read them anyway would be reading the next field along.
        for (std::uint32_t profile : {66u, 77u, 88u}) {
            const std::vector<std::uint8_t> sps = made_up_sps(profile);
            const mp::mft::SpsInfo got = parse_sps(sps.data(), sps.size());
            INFO("profile " << profile);
            REQUIRE(got.valid);
            CHECK(got.profile_idc == profile);
            CHECK(got.chroma_format_idc == 1u);
            CHECK(got.bit_depth_luma == 8u);
            CHECK(got.bit_depth_chroma == 8u);
        }
    }

    SECTION("High states them, and states 4:2:0 at eight")
    {
        const std::vector<std::uint8_t> sps = made_up_sps(100, 1, 0, 0);
        const mp::mft::SpsInfo got = parse_sps(sps.data(), sps.size());
        REQUIRE(got.valid);
        CHECK(got.chroma_format_idc == 1u);
        CHECK(got.bit_depth_luma == 8u);
    }

    SECTION("the shapes nothing here decodes")
    {
        struct Case {
            std::uint32_t profile;
            std::uint32_t chroma;
            std::uint32_t luma_minus8;
            std::uint32_t want_chroma;
            std::uint32_t want_bits;
        };
        // High 10, High 4:2:2, High 4:4:4 Predictive, and 4:4:4 at twelve --
        // which is the corner of H.264 a grading tool actually meets.
        const Case cases[] = {{110u, 1u, 2u, 1u, 10u},
                              {122u, 2u, 2u, 2u, 10u},
                              {244u, 3u, 0u, 3u, 8u},
                              {244u, 3u, 4u, 3u, 12u}};
        for (const Case& c : cases) {
            const std::vector<std::uint8_t> sps =
                made_up_sps(c.profile, c.chroma, c.luma_minus8, c.luma_minus8);
            const mp::mft::SpsInfo got = parse_sps(sps.data(), sps.size());
            INFO("profile " << c.profile << " chroma " << c.chroma);
            REQUIRE(got.valid);
            CHECK(got.chroma_format_idc == c.want_chroma);
            CHECK(got.bit_depth_luma == c.want_bits);
            CHECK(got.bit_depth_chroma == c.want_bits);
        }
    }

    SECTION("emulation prevention bytes are taken back out")
    {
        // **The failure this guards against is a plausible wrong answer.** A
        // NAL unit may not contain 00 00 00..03, so an encoder inserts a 03
        // after any 00 00 that would make one -- and a reader that does not
        // remove it shifts every bit after the first occurrence by eight.
        std::vector<std::uint8_t> sps = made_up_sps(100, 3, 4, 4);
        const mp::mft::SpsInfo clean = parse_sps(sps.data(), sps.size());
        REQUIRE(clean.valid);

        // Put a 00 00 03 in, after the fields under test, and check nothing
        // moved. The 03 is what an encoder would have written.
        sps.insert(sps.end(), {0x00, 0x00, 0x03, 0x01});
        const mp::mft::SpsInfo escaped = parse_sps(sps.data(), sps.size());
        REQUIRE(escaped.valid);
        CHECK(escaped.chroma_format_idc == clean.chroma_format_idc);
        CHECK(escaped.bit_depth_luma == clean.bit_depth_luma);
    }

    SECTION("what it declines")
    {
        // A PPS rather than an SPS: nal_unit_type 8.
        std::vector<std::uint8_t> pps = made_up_sps(100);
        pps[0] = 0x68;
        CHECK_FALSE(parse_sps(pps.data(), pps.size()).valid);

        // Truncated in the middle of the fields it reads.
        const std::vector<std::uint8_t> sps = made_up_sps(244, 3, 4, 4);
        CHECK_FALSE(parse_sps(sps.data(), 3).valid);

        CHECK_FALSE(parse_sps(nullptr, 16).valid);

        // An Exp-Golomb code longer than a u32 can hold is a corrupt file
        // asking for a shift nobody should perform.
        const std::vector<std::uint8_t> zeros(16, 0x00);
        std::vector<std::uint8_t> runaway{0x67};
        runaway.insert(runaway.end(), zeros.begin(), zeros.end());
        CHECK_FALSE(parse_sps(runaway.data(), runaway.size()).valid);
    }
}

TEST_CASE("codec_mft declines the H.264 it cannot decode, before it is opened",
          "[codec][mft][sps]")
{
    // **§7's rule needs this to happen at probe.** A decoder that fails
    // mid-file must not trigger a silent retry with another backend, so the
    // declining has to be early enough that a host can still look elsewhere --
    // and the `avcC` carries the answer before anything is opened.
    Module codec_module{MEDIAPERCH_CODEC_MFT, MP_KIND_VCODEC};
    REQUIRE(codec_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);

    const auto score_for = [&](const std::vector<std::uint8_t>& sps) {
        const std::vector<std::uint8_t> config = avcc_with(sps);
        std::uint32_t score = 0;
        REQUIRE(codec->probe(MP_CODEC_H264, MP_GRAPHICS_NONE, config.data(),
                             static_cast<std::uint32_t>(config.size()), &score) == MP_OK);
        return score;
    };

    // What it does decode.
    CHECK(score_for(made_up_sps(66)) == 80u);          // Baseline
    CHECK(score_for(made_up_sps(100, 1, 0, 0)) == 80u); // High, 4:2:0 8-bit

    // And what it does not. Measured rather than assumed: enumerating this
    // machine's D3D11 decoder profiles gives H264_VLD_NOFGT and no 4:2:2 or
    // 4:4:4 entry, and the software transform is 4:2:0 too.
    CHECK(score_for(made_up_sps(110, 1, 2, 2)) == 0u); // High 10
    CHECK(score_for(made_up_sps(122, 2, 0, 0)) == 0u); // High 4:2:2
    CHECK(score_for(made_up_sps(244, 3, 0, 0)) == 0u); // High 4:4:4 Predictive
    CHECK(score_for(made_up_sps(244, 3, 4, 4)) == 0u); // and at twelve bits

    // **A config it cannot read is not evidence of a format it cannot decode.**
    // Refusing on "I could not tell" would decline files that work, so a
    // missing or unparseable record leaves the score where it was.
    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_H264, MP_GRAPHICS_NONE, nullptr, 0, &score) == MP_OK);
    CHECK(score == 80u);

    // Opening one anyway is refused with a sentence rather than a wrong
    // picture -- the second line of defence, the way the api check is.
    const std::vector<std::uint8_t> config = avcc_with(made_up_sps(244, 3, 4, 4));
    MpVideoCodec* decoder = nullptr;
    CHECK(codec->open(MP_CODEC_H264, nullptr, config.data(),
                      static_cast<std::uint32_t>(config.size()),
                      &decoder) == MP_ERR_UNSUPPORTED);
}


TEST_CASE("an avcC is read, or refused, and never guessed at", "[video][avcc]")
{
    using namespace mp::mft;

    const std::vector<std::uint8_t> raw = made_up_avcc();
    const AvcConfig config = parse_avcc(raw.data(), raw.size());
    REQUIRE(config.valid);

    // **lengthSizeMinusOne, plus one.** The field is named for the value it is
    // one less than, which is the sort of thing that reads correctly and is
    // wrong by one for a year.
    CHECK(config.length_size == 4);
    REQUIRE(config.parameter_sets.size() == 2);
    CHECK(config.parameter_sets[0].size() == 4); // the SPS
    CHECK(config.parameter_sets[1].size() == 3); // the PPS

    SECTION("a truncated record is refused rather than half-read")
    {
        for (std::size_t cut = 0; cut < raw.size(); ++cut) {
            const AvcConfig partial = parse_avcc(raw.data(), cut);
            CHECK_FALSE(partial.valid);
        }
    }

    SECTION("and so is a length size the spec forbids")
    {
        // lengthSizeMinusOne of 2, meaning three-byte lengths, which ISO/IEC
        // 14496-15 does not allow. A file that says it is a file to decline.
        const std::vector<std::uint8_t> bad = made_up_avcc(2);
        CHECK_FALSE(parse_avcc(bad.data(), bad.size()).valid);
    }

    SECTION("a record with no parameter sets is not usable")
    {
        std::vector<std::uint8_t> empty = {0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE0, 0x00};
        CHECK_FALSE(parse_avcc(empty.data(), empty.size()).valid);
    }
}

TEST_CASE("AVCC samples become Annex B, with the parameter sets in front",
          "[video][avcc]")
{
    using namespace mp::mft;

    const std::vector<std::uint8_t> raw = made_up_avcc();
    const AvcConfig config = parse_avcc(raw.data(), raw.size());
    REQUIRE(config.valid);

    // Two NAL units, four-byte lengths, as MP4 stores them.
    const std::vector<std::uint8_t> sample = {
        0x00, 0x00, 0x00, 0x02, 0x41, 0x9A,             // a two-byte slice
        0x00, 0x00, 0x00, 0x03, 0x41, 0x9B, 0x9C,       // a three-byte one
    };

    std::vector<std::uint8_t> out;
    REQUIRE(to_annex_b(config, sample.data(), sample.size(), true, out));

    // SPS, PPS, then the two slices, each behind a four-byte start code.
    const std::vector<std::uint8_t> expected = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0xE3,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9A,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9B, 0x9C,
    };
    CHECK(out == expected);

    SECTION("and without them when the decoder already has them")
    {
        std::vector<std::uint8_t> bare;
        REQUIRE(to_annex_b(config, sample.data(), sample.size(), false, bare));
        CHECK(bare.size() == expected.size() - 15u); // the two parameter sets
        CHECK(bare[4] == 0x41);
    }

    SECTION("a length that runs past the end is a truncated sample, not a frame")
    {
        // **The check that matters**, because the alternative is handing a
        // decoder a NAL unit that stops in the middle of a slice.
        std::vector<std::uint8_t> broken = sample;
        broken[3] = 0x40; // says 64 bytes, has 2
        std::vector<std::uint8_t> ignored;
        CHECK_FALSE(to_annex_b(config, broken.data(), broken.size(), true, ignored));

        // And a sample whose last length header does not even fit.
        CHECK_FALSE(to_annex_b(config, sample.data(), sample.size() - 4, true, ignored));
    }

    SECTION("an empty NAL unit is dropped rather than given a start code")
    {
        const std::vector<std::uint8_t> with_empty = {0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x00, 0x01, 0x41};
        std::vector<std::uint8_t> got;
        REQUIRE(to_annex_b(config, with_empty.data(), with_empty.size(), false, got));
        CHECK(got == std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x41});
    }
}

TEST_CASE("the H.264 track of a real MP4 decodes to frames", "[video][mft]")
{
    // **End to end, through the split §9.8 argues for**: `demux_mp4` reads the
    // container and hands over the sample bytes and the `avcC` verbatim;
    // `codec_mft` converts the bitstream and decodes it. Media Foundation never
    // sees the file, never seeks, and never says what is in it.
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    REQUIRE(demux_module.vtbl != nullptr);
    Module codec_module{MEDIAPERCH_CODEC_MFT, MP_KIND_VCODEC};
    REQUIRE(codec_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV) == MP_OK);

    std::uint32_t video = 0;
    bool found = false;
    MpStreamInfo info{};
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        if (demux.stream_info(i, info) && info.kind == MP_STREAM_VIDEO) {
            video = i;
            found = true;
            break;
        }
    }
    REQUIRE(found);

    // The container names the codec now, where it used to say "unknown".
    CHECK(info.codec == MP_CODEC_H264);
    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(video, config));
    CHECK(config.size() > 7); // an avcC with something in it

    // **No device: system memory, deterministic, and available on a machine
    // with no GPU at all.** The hardware path takes the presenter's device and
    // hands back a texture; this is the one a test can run anywhere.
    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_H264, MP_GRAPHICS_NONE, config.data(),
                         static_cast<std::uint32_t>(config.size()), &score) == MP_OK);
    CHECK(score != 0);

    // And it declines a device it cannot decode onto, which is the whole
    // reason `probe` asks: Media Foundation binds to an ID3D11Device and has
    // no D3D12 form, so a D3D12 presenter needs a different module.
    std::uint32_t d3d12_score = 0;
    REQUIRE(codec->probe(MP_CODEC_H264, MP_GRAPHICS_D3D12, config.data(),
                         static_cast<std::uint32_t>(config.size()),
                         &d3d12_score) == MP_OK);
    CHECK(d3d12_score == 0);

    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_H264, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);
    REQUIRE(decoder != nullptr);

    const std::uint32_t only_video[] = {video};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t packets = 0;
    std::uint32_t frames = 0;
    std::uint32_t widest = 0;
    bool any_content = false;

    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec->next_frame(decoder, &frame);
            if (r == MP_END) {
                return;
            }
            if (r == MP_ERR_BUSY) {
                continue; // the stream changed; ask again
            }
            REQUIRE(r == MP_OK);
            ++frames;
            widest = std::max(widest, frame.width);
            CHECK(frame.height == 96u);
            // System memory, so planes rather than a texture.
            REQUIRE(frame.texture == nullptr);
            REQUIRE(frame.plane[0] != nullptr);
            REQUIRE(frame.stride[0] >= frame.width);
            // testsrc2 is not a flat colour, so the luma plane is not one
            // value repeated -- which is the cheapest evidence that something
            // was actually decoded rather than a buffer handed back cleared.
            const auto* luma = static_cast<const std::uint8_t*>(frame.plane[0]);
            for (std::uint32_t x = 1; x < frame.width; ++x) {
                if (luma[x] != luma[0]) {
                    any_content = true;
                    break;
                }
            }
        }
    };

    for (int guard = 0; guard < 500; ++guard) {
        const MpResult r = demux.read_packet(buffer, packet);
        if (r == MP_END) {
            break;
        }
        REQUIRE(r == MP_OK);
        ++packets;
        const MpResult fed = codec->decode(decoder, buffer.data(), packet.bytes, packet.frame);
        if (fed == MP_ERR_BUSY) {
            drain();
            REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) ==
                    MP_OK);
        } else {
            REQUIRE(fed == MP_OK);
        }
        drain();
    }
    CHECK(packets == 24);

    // What is still inside. A decoder holds frames back to reorder them, so a
    // caller that stopped at the last packet would lose the tail of every file.
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();

    CHECK(frames == 24);
    CHECK(widest == 128u);
    CHECK(any_content);

    MpVideoInfo produced{};
    produced.size = sizeof(produced);
    REQUIRE(codec->get_format(decoder, &produced) == MP_OK);
    CHECK(produced.width == 128u);
    CHECK(produced.height == 96u);
    // **Hundred-nanosecond units, stated rather than assumed**, which is §9.9's
    // whole point: a timestamp with no stated unit is a number nobody can read.
    CHECK(produced.timescale == 10000000u);

    codec->close(decoder);
}

TEST_CASE("a decoded frame reaches the presenter and comes back as pixels",
          "[video][mft][d3d11]")
{
    // **The whole chain, and the first picture this project has produced.**
    // demux_mp4 reads the container, codec_mft decodes the bitstream to NV12 in
    // system memory, video_d3d11 converts it by the matrix the container named
    // and renders it, and read_back hands over single-precision linear light
    // that a test can look at. Nothing here is looked at by a person.
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_MFT, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    REQUIRE(video_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);
    const auto* video = static_cast<const MpVideoVtbl*>(video_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV) == MP_OK);
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

    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_H264, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    // **The container's colour, not the decoder's.** demux_mp4 read `colr`;
    // codec_mft deliberately reports unspecified because it knows less. This is
    // the join, and getting it the wrong way round is how a picture ends up
    // merely plausible.
    MpVideoInfo geometry{};
    geometry.size = sizeof(geometry);
    REQUIRE(demux.video_info(stream, geometry));

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
            if (r == MP_ERR_BUSY) {
                continue;
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
    CHECK(presented == 24);

    // The last frame, in linear light at single precision.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    MpPixelLayout layout{};
    layout.size = sizeof(layout);
    REQUIRE(video->read_back(presenter, nullptr, 0, &width, &height, &layout) ==
            MP_ERR_NO_MEMORY);
    CHECK(width == 128u);
    CHECK(height == 96u);
    CHECK(layout.chroma == MP_CHROMA_RGB);
    CHECK(layout.packing == MP_PACK_INTERLEAVED);
    CHECK((layout.flags & MP_PIXEL_FLOAT) != 0u);
    REQUIRE(layout.container_bits == 32u);

    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4u);
    REQUIRE(video->read_back(presenter, pixels.data(), pixels.size() * sizeof(float),
                             &width, &height, &layout) == MP_OK);

    // **A picture, and this is what says so.** testsrc2 is bars and shapes, so
    // a frame of it has a spread of luminance and more than one hue -- neither
    // of which a cleared buffer, a stuck decoder or a dropped chroma plane
    // would produce.
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
        // Linear light out of an SDR frame stays inside the range the transfer
        // is defined over; anything outside it is arithmetic that went wrong
        // rather than a highlight.
        CHECK(r >= -0.001f);
        CHECK(r <= 1.001f);
    }
    CHECK(darkest < 0.2f);
    CHECK(brightest > 0.5f);
    CHECK(coloured);

    video->close(presenter);
    codec->close(decoder);
}

TEST_CASE("a hardware decoder decodes into a texture the presenter samples in place",
          "[video][mft][d3d11][hardware]")
{
    // **The zero-copy path, on whatever GPU this machine actually has.**
    //
    // Every other test here asks the presenter for WARP, because WARP is
    // deterministic and a hash of its pixels means the same thing everywhere.
    // The cost of that choice is precisely this case: WARP has no video device
    // at all -- no `ID3D11VideoDevice`, no decoder profiles, and asking it for
    // `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` fails outright -- so the tests that
    // ask for it cannot see whether Media Foundation grants the binding
    // `codec_mft` requests through `MF_SA_D3D11_BINDFLAGS`. That is a fact
    // about WARP and not about the machine, and this test is the machine's.
    //
    // So: the hardware device, the fixture decoded on it, and a look at the
    // texture that comes back. It skips rather than fails where there is no
    // hardware adapter, and says so -- a test that quietly does nothing is
    // also a claim.
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_MFT, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.vtbl != nullptr);
    REQUIRE(codec_module.vtbl != nullptr);
    REQUIRE(video_module.vtbl != nullptr);
    const auto* codec = static_cast<const MpVideoCodecVtbl*>(codec_module.vtbl);
    const auto* video = static_cast<const MpVideoVtbl*>(video_module.vtbl);

    mp::Demux demux;
    REQUIRE(demux.open(*static_cast<const MpDemuxVtbl*>(demux_module.vtbl),
                       MEDIAPERCH_TEST_AV) == MP_OK);
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
    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(stream, config));
    MpVideoInfo geometry{};
    geometry.size = sizeof(geometry);
    REQUIRE(demux.video_info(stream, geometry));

    // No `device` setting, which asks for the hardware adapter and falls back
    // to WARP -- and the fallback is what the skip below is looking for.
    MpVideo* presenter = nullptr;
    REQUIRE(video->open(nullptr, &presenter) == MP_OK);
    REQUIRE(video->configure(presenter, &geometry) == MP_OK);

    MpGraphicsDevice graphics{};
    graphics.size = sizeof(graphics);
    REQUIRE(video->get_device(presenter, &graphics) == MP_OK);
    REQUIRE(graphics.api == MP_GRAPHICS_D3D11);
    REQUIRE(graphics.device != nullptr);

    // Asked of the device rather than of the setting, because this is the
    // property that matters: an adapter with no video engine is the same case
    // as WARP, and neither can decode.
    ID3D11VideoDevice* video_device = nullptr;
    if (FAILED(static_cast<ID3D11Device*>(graphics.device)
                   ->QueryInterface(IID_PPV_ARGS(&video_device)))) {
        video->close(presenter);
        SKIP("the presenter's device has no ID3D11VideoDevice, so there is nothing "
             "to decode with -- WARP, a remote session, or an adapter with no video "
             "engine");
    }
    const UINT profiles = video_device->GetVideoDecoderProfileCount();
    video_device->Release();
    CHECK(profiles > 0);

    std::uint32_t score = 0;
    REQUIRE(codec->probe(MP_CODEC_H264, MP_GRAPHICS_D3D11, config.data(),
                         static_cast<std::uint32_t>(config.size()), &score) == MP_OK);
    CHECK(score > 0);

    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_H264, &graphics, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    const std::uint32_t only_video[] = {stream};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t frames = 0;
    std::uint32_t textures = 0;
    std::uint32_t presented = 0;
    bool granted = true;
    bool looked = false;
    std::string shape;

    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            const MpResult r = codec->next_frame(decoder, &frame);
            if (r == MP_END) {
                return;
            }
            if (r == MP_ERR_BUSY) {
                continue;
            }
            REQUIRE(r == MP_OK);
            ++frames;
            if (frame.texture == nullptr) {
                REQUIRE(video->present(presenter, &frame) == MP_OK);
                ++presented;
                continue;
            }
            ++textures;
            if (!looked) {
                looked = true;
                D3D11_TEXTURE2D_DESC desc{};
                static_cast<ID3D11Texture2D*>(frame.texture)->GetDesc(&desc);
                char line[256];
                std::snprintf(line, sizeof(line),
                              "%ux%u DXGI format %u, an array of %u slices, this frame "
                              "at %u, bind flags 0x%X",
                              desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                              desc.ArraySize, frame.texture_index, desc.BindFlags);
                shape = line;
                // A decoder hands out one array and an index into it, which is
                // why the frame carries both.
                CHECK(desc.ArraySize >= 1u);
                CHECK(frame.texture_index < desc.ArraySize);
                CHECK((desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010));
                CHECK((desc.BindFlags & D3D11_BIND_DECODER) != 0);
                granted = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
            }
            // **Both answers are correct behaviour and only one is silence.**
            // A driver that honoured MF_SA_D3D11_BINDFLAGS gets sampled in
            // place; one that declined has to be told about by name rather
            // than copied around quietly, which is what the refusal is.
            if (granted) {
                INFO(shape);
                REQUIRE(video->present(presenter, &frame) == MP_OK);
                ++presented;
            } else {
                CHECK(video->present(presenter, &frame) == MP_ERR_UNSUPPORTED);
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

    codec->close(decoder);

    if (textures == 0) {
        video->close(presenter);
        SKIP("this machine has a video device but its decoder would not take it; "
             "the frames came back in system memory, which the other tests cover");
    }
    INFO(shape);
    CHECK(textures == frames);

    if (!granted) {
        char why[512] = "";
        video->describe(presenter, 0, why, sizeof(why));
        video->close(presenter);
        SKIP("this machine's decoder would not grant D3D11_BIND_SHADER_RESOURCE, so "
             "the presenter refused the texture by name, which is the other half of "
             "what MF_SA_D3D11_BINDFLAGS is for");
    }
    CHECK(presented == frames);

    // **And the picture, so that "adopted" means more than "not refused".**
    // The same evidence the system-memory chain is held to: testsrc2 is bars
    // and shapes, so a frame of it has a spread of luminance and more than one
    // hue, and a stuck decoder or a dropped chroma plane has neither.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    MpPixelLayout layout{};
    layout.size = sizeof(layout);
    REQUIRE(video->read_back(presenter, nullptr, 0, &width, &height, &layout) ==
            MP_ERR_NO_MEMORY);
    CHECK(width == 128u);
    CHECK(height == 96u);
    CHECK(layout.chroma == MP_CHROMA_RGB);
    CHECK(layout.packing == MP_PACK_INTERLEAVED);
    CHECK((layout.flags & MP_PIXEL_FLOAT) != 0u);
    REQUIRE(layout.container_bits == 32u);

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
        darkest = std::min(darkest, 0.2126f * r + 0.7152f * g + 0.0722f * b);
        brightest = std::max(brightest, 0.2126f * r + 0.7152f * g + 0.0722f * b);
        if (std::abs(r - b) > 0.05f || std::abs(g - b) > 0.05f) {
            coloured = true;
        }
    }
    CHECK(darkest < 0.2f);
    CHECK(brightest > 0.5f);
    CHECK(coloured);

    video->close(presenter);
}

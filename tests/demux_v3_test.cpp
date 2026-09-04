// SPDX-License-Identifier: GPL-3.0-or-later
//
// ABI v3, against a real demuxer reading a real file with two streams in it.
//
// **This is the test the break exists for.** v2's `select(index)` named one
// stream and `seek(frame)` meant "the selected one", which is fine for every
// audio container in this tree and has no answer at all once a player wants
// audio and video out of one file. Appending would have left `select` meaning
// something narrower than its name and `seek` as a trap; so two members changed
// in place, `MpPacket::reserved` became `stream`, and `MP_ABI_VERSION` went to
// 3 -- which makes every module fail to load until it is rebuilt, and every
// module in the world is in this repository.
//
// Everything else in `tests/` drives fakes, deliberately: a fake says exactly
// what the host is being tested against. This one does not, because what is
// under test is whether the *shape* works on a container somebody else's tool
// wrote, and a fake that interleaved the way I imagined MP4 interleaves would
// prove nothing at all. So it loads `mp_demux_mp4.dll` through the module
// registry and reads `tests/data/mp4/av.mp4`, which FFmpeg wrote.

#include "mediaperch/packet.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/// The module and the file, both passed in by CMake: a test that went looking
/// for either would be testing the search.
const char* module_path()
{
    return MEDIAPERCH_DEMUX_MP4;
}

const char* av_path()
{
    return MEDIAPERCH_TEST_AV;
}

const char* bt2020_path()
{
    return MEDIAPERCH_TEST_AV_BT2020;
}

/// The demuxer's vtable, loaded the way the engine loads it.
///
/// Deliberately not through `mp::win::ModuleRegistry`: what is under test is
/// the portable half against a module, and the registry belongs to the Windows
/// head. What a module *is*, is a DLL exporting `mp_module_entry` -- and saying
/// so in six lines is a better test of the ABI than borrowing the loader that
/// already knows.
struct Module {
    Module()
    {
        auto* dll = ::LoadLibraryA(module_path());
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
        // **The version check is the break, made visible.** A module built
        // against v2 answers null here, which is exactly what a bump is for.
        const MpModuleDesc* desc = entry(MP_ABI_VERSION);
        if (desc == nullptr || desc->kind != MP_KIND_DEMUX) {
            return;
        }
        vtbl = static_cast<const MpDemuxVtbl*>(desc->vtbl);
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
    const MpDemuxVtbl* vtbl = nullptr;
};

/// Which stream is which in the fixture, found rather than assumed.
struct Streams {
    std::uint32_t audio = 0;
    std::uint32_t video = 0;
    bool both = false;
};

Streams find(const mp::Demux& demux)
{
    Streams out{};
    bool audio = false;
    bool video = false;
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        MpStreamInfo info{};
        if (!demux.stream_info(i, info)) {
            continue;
        }
        if (info.kind == MP_STREAM_AUDIO && !audio) {
            out.audio = i;
            audio = true;
        }
        if (info.kind == MP_STREAM_VIDEO && !video) {
            out.video = i;
            video = true;
        }
    }
    out.both = audio && video;
    return out;
}

} // namespace

TEST_CASE("one demuxer serves two streams out of one file", "[abi][v3][demux]")
{
    Module module;
    REQUIRE(module.vtbl != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.vtbl, av_path()) == MP_OK);
    REQUIRE(demux.stream_count() == 2);

    const Streams at = find(demux);
    REQUIRE(at.both);

    SECTION("selected together, they arrive interleaved and each says which it is")
    {
        const std::uint32_t both[] = {at.audio, at.video};
        REQUIRE(demux.select_streams(both) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint32_t audio_packets = 0;
        std::uint32_t video_packets = 0;
        // True the moment a packet of one stream follows a packet of the other.
        // **This is the property, and nothing in v2 could express it**: two
        // streams coming out of one pass over one file.
        bool interleaved = false;
        std::uint32_t previous = 0xFFFFFFFFu;

        for (int guard = 0; guard < 10000; ++guard) {
            const MpResult r = demux.read_packet(buffer, packet);
            if (r == MP_END) {
                break;
            }
            REQUIRE(r == MP_OK);
            REQUIRE((packet.stream == at.audio || packet.stream == at.video));
            if (previous != 0xFFFFFFFFu && packet.stream != previous) {
                interleaved = true;
            }
            previous = packet.stream;
            if (packet.stream == at.audio) {
                ++audio_packets;
            } else {
                ++video_packets;
            }
        }

        // What ffprobe says the file holds: 45 AAC frames and 24 H.264 frames.
        CHECK(audio_packets == 45);
        CHECK(video_packets == 24);
        CHECK(interleaved);
    }

    SECTION("selected alone, only that stream comes back")
    {
        const std::uint32_t only_audio[] = {at.audio};
        REQUIRE(demux.select_streams(only_audio) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint32_t packets = 0;
        for (int guard = 0; guard < 10000; ++guard) {
            const MpResult r = demux.read_packet(buffer, packet);
            if (r == MP_END) {
                break;
            }
            REQUIRE(r == MP_OK);
            CHECK(packet.stream == at.audio);
            ++packets;
        }
        CHECK(packets == 45);
    }

    SECTION("an empty set is refused, because it never had a caller")
    {
        // It nearly had a meaning -- "how a host stops reading" -- and then
        // five single-stream demuxers did not implement it, which would have
        // been the header lying. A host that has stopped reading closes the
        // demuxer. Loosening this later is not a break.
        CHECK(demux.select_streams({}) == MP_ERR_INVALID);
    }

    SECTION("a stream that is not there is refused, and so is one named twice")
    {
        const std::uint32_t nonexistent[] = {7};
        CHECK(demux.select_streams(nonexistent) == MP_ERR_INVALID);
        const std::uint32_t twice[] = {at.audio, at.audio};
        CHECK(demux.select_streams(twice) == MP_ERR_INVALID);
    }
}

TEST_CASE("a seek names the stream its frame is counted in", "[abi][v3][demux]")
{
    // v2's `seek(frame)` meant "the selected stream" and had no answer once two
    // were selected. **A host seeking by the audio clock names the audio
    // stream** -- and `frame` is in that stream's own rate, which is 44100 here
    // and 24000/1001 for the video, so the two numbers are not interchangeable.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.vtbl, av_path()) == MP_OK);
    const Streams at = find(demux);
    REQUIRE(at.both);

    const std::uint32_t both[] = {at.audio, at.video};
    REQUIRE(demux.select_streams(both) == MP_OK);

    // Half a second in, counted in audio frames.
    REQUIRE(demux.seek(at.audio, 22050) == MP_OK);

    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    bool saw_audio = false;
    bool saw_video = false;
    std::uint64_t first_audio_frame = 0;
    for (int guard = 0; guard < 200 && !(saw_audio && saw_video); ++guard) {
        if (demux.read_packet(buffer, packet) != MP_OK) {
            break;
        }
        if (packet.stream == at.audio && !saw_audio) {
            saw_audio = true;
            first_audio_frame = packet.frame;
        }
        if (packet.stream == at.video) {
            saw_video = true;
        }
    }

    // **One file has one position, so the seek moved both.** The video comes
    // from wherever its own nearest point was, which is not the audio's point
    // and is not required to be.
    CHECK(saw_audio);
    CHECK(saw_video);

    // At or before the target, never after it: landing early costs the host a
    // discard and landing late loses audio it can never get back.
    CHECK(first_audio_frame <= 22050);

    // And the stream index is checked rather than ignored.
    CHECK(demux.seek(9, 0) == MP_ERR_INVALID);
}

TEST_CASE("the container's colour is read rather than guessed", "[abi][v3][video]")
{
    // The three code points are the reason `stream_video_info` exists. Nothing
    // else says whether the frames are BT.709 or BT.2020, or whether the
    // transfer is sRGB or PQ, and §9.1 turns on knowing before a frame is drawn.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    SECTION("geometry, and the frame rate as the ratio it is")
    {
        mp::Demux demux;
        REQUIRE(demux.open(*module.vtbl, av_path()) == MP_OK);
        const Streams at = find(demux);
        REQUIRE(at.both);

        MpVideoInfo info{};
        REQUIRE(demux.video_info(at.video, info));
        CHECK(info.width == 128);
        CHECK(info.height == 96);
        CHECK(info.display_width == 128);
        CHECK(info.display_height == 96);
        // 24000/1001, not 23.976: rounding it is how a player drifts a frame
        // every seventeen minutes.
        CHECK(info.fps_num == 24000);
        CHECK(info.fps_den == 1001);

        // What FFmpeg actually wrote, which is not what it was asked for --
        // 2 is "unspecified" and is the common case a renderer has to handle.
        CHECK(info.primaries == 2);
        CHECK(info.transfer == 2);
        CHECK(info.matrix == 2);
        CHECK((info.flags & MP_VIDEO_FULL_RANGE) == 0);

        // And an audio stream has no video information, which is a different
        // answer from "this module has none".
        MpVideoInfo audio{};
        CHECK_FALSE(demux.video_info(at.audio, audio));
    }

    SECTION("BT.2020 and PQ come back as BT.2020 and PQ")
    {
        // The same file with four bytes changed in its `colr` box -- see
        // tools/make_av_fixture.py for why it is made by hand. Everything else
        // about the two files is identical, so a difference here is the box
        // being read and nothing else.
        mp::Demux demux;
        REQUIRE(demux.open(*module.vtbl, bt2020_path()) == MP_OK);
        const Streams at = find(demux);
        REQUIRE(at.both);

        MpVideoInfo info{};
        REQUIRE(demux.video_info(at.video, info));
        CHECK(info.primaries == 9);  // BT.2020
        CHECK(info.transfer == 16);  // SMPTE ST.2084, which is PQ
        CHECK(info.matrix == 9);     // BT.2020 non-constant luminance
        CHECK((info.flags & MP_VIDEO_FULL_RANGE) != 0);

        // The geometry did not move, which is what makes the four bytes the
        // only difference.
        CHECK(info.width == 128);
        CHECK(info.height == 96);
    }
}

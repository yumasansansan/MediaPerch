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

/// The modules and the files, all passed in by CMake: a test that went looking
/// for any of them would be testing the search.
const char* mp4_module()
{
    return MEDIAPERCH_DEMUX_MP4;
}

const char* mkv_module()
{
    return MEDIAPERCH_DEMUX_MKV;
}

const char* av_path()
{
    return MEDIAPERCH_TEST_AV;
}

const char* bt2020_path()
{
    return MEDIAPERCH_TEST_AV_BT2020;
}

const char* mkv_path()
{
    return MEDIAPERCH_TEST_AV_MKV;
}

/// The demuxer's vtable, loaded the way the engine loads it.
///
/// Deliberately not through `mp::win::ModuleRegistry`: what is under test is
/// the portable half against a module, and the registry belongs to the Windows
/// head. What a module *is*, is a DLL exporting `mp_module_entry` -- and saying
/// so in six lines is a better test of the ABI than borrowing the loader that
/// already knows.
struct Module {
    explicit Module(const char* path)
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
    Module module{mp4_module()};
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

TEST_CASE("AV1 in an MP4 is named, and its av1C comes across verbatim",
          "[abi][v3][demux][av1]")
{
    // **The container half of AV1, which is where it starts.** Nothing in this
    // tree decodes AV1 yet; `demux_mp4` still has to recognise an `av01` sample
    // entry and hand over the `av1C` record, and that is checkable on its own.
    // Same picture, same length and same audio as `av.mp4` -- SVT-AV1 instead
    // of x264 -- so the two files differ in the codec and in nothing else.
    Module module{mp4_module()};
    REQUIRE(module.vtbl != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.vtbl, MEDIAPERCH_TEST_AV1) == MP_OK);
    REQUIRE(demux.stream_count() == 2);

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
    CHECK(info.codec == MP_CODEC_AV1);

    MpVideoInfo geometry{};
    geometry.size = sizeof(geometry);
    REQUIRE(demux.video_info(video, geometry));
    CHECK(geometry.width == 128u);
    CHECK(geometry.height == 96u);

    // **The record, verbatim, and the first byte is what says so.** An
    // AV1CodecConfigurationRecord opens with a marker bit of 1 and a version of
    // 1 in the low seven bits, which is 0x81 -- and Bento4 keeps no raw bytes
    // for this box, so a record reassembled from its parsed fields would very
    // likely also start 0x81 and be this module's opinion of the file rather
    // than the file. The length is the second half of the check: a rebuilt
    // four-byte record would not carry the sequence header OBU that follows.
    std::vector<std::uint8_t> config;
    REQUIRE(demux.stream_config(video, config));
    REQUIRE(config.size() > 4u);
    CHECK(config[0] == 0x81u);
    // seq_profile is the top three bits of the second byte; Main is 0.
    CHECK((config[1] >> 5) == 0u);

    // And the packets come out. AV1 in an MP4 is already OBUs with their own
    // sizes, so unlike H.264 there is no length-prefixed framing for a decoder
    // to undo -- twenty-four samples, one a frame, the same as av.mp4.
    const std::uint32_t only_video[] = {video};
    REQUIRE(demux.select_streams(only_video) == MP_OK);
    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    std::uint32_t packets = 0;
    while (demux.read_packet(buffer, packet) == MP_OK) {
        CHECK(packet.stream == video);
        CHECK(packet.bytes > 0);
        ++packets;
    }
    CHECK(packets == 24u);
}

TEST_CASE("a seek names the stream its frame is counted in", "[abi][v3][demux]")
{
    // v2's `seek(frame)` meant "the selected stream" and had no answer once two
    // were selected. **A host seeking by the audio clock names the audio
    // stream** -- and `frame` is in that stream's own rate, which is 44100 here
    // and 24000/1001 for the video, so the two numbers are not interchangeable.
    Module module{mp4_module()};
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
    Module module{mp4_module()};
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

// --------------------------------------------------------------------------
// The same shape, in a container that interleaves differently
// --------------------------------------------------------------------------

TEST_CASE("Matroska serves two tracks out of one pass as well", "[abi][v3][demux]")
{
    // **A second container is what makes v3 an interface rather than one
    // module's habit.** MP4 stores samples in a flat table with a linear reader
    // that already knew how to walk several tracks; Matroska stores blocks
    // inside clusters, laced several frames to a block, and the reader here had
    // a lace cursor and a frame rate that were both the one selected track's.
    // Whether the ABI's shape survives that is the question, and it is not the
    // same question MP4 answered.
    Module module{mkv_module()};
    REQUIRE(module.vtbl != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.vtbl, mkv_path()) == MP_OK);
    REQUIRE(demux.stream_count() == 2);

    const Streams at = find(demux);
    REQUIRE(at.both);

    SECTION("both tracks, interleaved, each saying which it is")
    {
        const std::uint32_t both[] = {at.audio, at.video};
        REQUIRE(demux.select_streams(both) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint32_t audio_packets = 0;
        std::uint32_t video_packets = 0;
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

        // What ffprobe counts in the file: 51 Opus packets and 24 H.264 frames.
        // **Packets and not blocks**: Matroska laces several frames into one
        // block, so a demuxer that counted blocks would be short here and
        // nowhere else.
        CHECK(audio_packets == 51);
        CHECK(video_packets == 24);
        CHECK(interleaved);
    }

    SECTION("one track, and the other one is not in it")
    {
        const std::uint32_t only_video[] = {at.video};
        REQUIRE(demux.select_streams(only_video) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint32_t packets = 0;
        for (int guard = 0; guard < 10000; ++guard) {
            const MpResult r = demux.read_packet(buffer, packet);
            if (r == MP_END) {
                break;
            }
            REQUIRE(r == MP_OK);
            CHECK(packet.stream == at.video);
            ++packets;
        }
        CHECK(packets == 24);
    }

    SECTION("a timestamp means two different frame numbers, and each track gets its own")
    {
        // The reason `frames_of` takes a track index now. One cluster timestamp
        // is 48 kHz frames to the Opus track and something else entirely to the
        // video, and a reader with one idea of the rate answers the same number
        // for both.
        const std::uint32_t both[] = {at.audio, at.video};
        REQUIRE(demux.select_streams(both) == MP_OK);

        MpStreamInfo audio_info{};
        REQUIRE(demux.stream_info(at.audio, audio_info));
        REQUIRE(audio_info.format.sample_rate != 0);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint64_t last_audio = 0;
        std::uint32_t timed_audio = 0;
        for (int guard = 0; guard < 10000; ++guard) {
            if (demux.read_packet(buffer, packet) != MP_OK) {
                break;
            }
            if (packet.stream != at.audio || (packet.flags & MP_PACKET_TIMED) == 0) {
                continue;
            }
            CHECK(packet.frame >= last_audio); // monotonic, in the audio's rate
            last_audio = packet.frame;
            ++timed_audio;
        }
        CHECK(timed_audio != 0);

        // **One second of audio, counted in the audio's rate.** 48048 rather
        // than 48000 because the file is 1.001 seconds long: 24 video frames at
        // 24000/1001, and the muxer matched the audio to it. A tenth of a
        // second of slack, which is the wrong sort of number to be off by if
        // the rate were the video track's -- that would answer 0 for every
        // packet, because a video track has no sample rate at all.
        CHECK(last_audio > audio_info.format.sample_rate / 2);
        CHECK(last_audio < audio_info.format.sample_rate * 11 / 10);

        // And the video's packets are timestamped too, in the units its own
        // MpVideoInfo states -- nanoseconds here. Before §9.9 was answered this
        // module declined to timestamp video at all, because the number had no
        // stated unit and MP_PACKET_TIMED on it would have been a claim nobody
        // could check.
        MpVideoInfo video{};
        REQUIRE(demux.video_info(at.video, video));
        REQUIRE(video.timescale == 1000000000u);

        REQUIRE(demux.seek(at.audio, 0) == MP_OK);
        std::uint64_t highest_video = 0;
        std::uint32_t timed_video = 0;
        for (int guard = 0; guard < 500; ++guard) {
            if (demux.read_packet(buffer, packet) != MP_OK) {
                break;
            }
            if (packet.stream != at.video || (packet.flags & MP_PACKET_TIMED) == 0) {
                continue;
            }
            highest_video = std::max(highest_video, packet.frame);
            ++timed_video;
        }
        CHECK(timed_video == 24);
        // One second of video, in nanoseconds. A number in the *audio's* units
        // would be forty-eight thousand rather than a billion, which is the
        // confusion the timescale exists to prevent.
        CHECK(highest_video > 900000000ull);
        CHECK(highest_video < 1100000000ull);
    }

    SECTION("a seek moves both, and is counted in the named track's rate")
    {
        const std::uint32_t both[] = {at.audio, at.video};
        REQUIRE(demux.select_streams(both) == MP_OK);

        MpStreamInfo audio_info{};
        REQUIRE(demux.stream_info(at.audio, audio_info));
        REQUIRE(demux.seek(at.audio, audio_info.format.sample_rate / 2) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        bool saw_audio = false;
        bool saw_video = false;
        for (int guard = 0; guard < 500 && !(saw_audio && saw_video); ++guard) {
            if (demux.read_packet(buffer, packet) != MP_OK) {
                break;
            }
            saw_audio = saw_audio || packet.stream == at.audio;
            saw_video = saw_video || packet.stream == at.video;
        }
        CHECK(saw_audio);
        CHECK(saw_video);

        CHECK(demux.seek(9, 0) == MP_ERR_INVALID);
    }

    SECTION("and the same refusals as everywhere else")
    {
        const std::uint32_t nonexistent[] = {7};
        CHECK(demux.select_streams(nonexistent) == MP_ERR_INVALID);
        const std::uint32_t twice[] = {at.video, at.video};
        CHECK(demux.select_streams(twice) == MP_ERR_INVALID);
        CHECK(demux.select_streams({}) == MP_ERR_INVALID);
    }
}

TEST_CASE("a video packet says what its timestamp is counted in", "[abi][v3][video]")
{
    // §9.9. `MpPacket::frame` used to be documented as the stream's own frames,
    // which an audio stream has and a video stream does not -- 24000/1001 of a
    // second is not a unit anything divides evenly. The two demuxers that read
    // video answered differently and neither could be checked: one declined to
    // timestamp video at all, the other handed back a number in a timescale
    // nothing revealed.
    SECTION("MP4 counts in the track's own timescale, which mdhd states")
    {
        Module module{mp4_module()};
        REQUIRE(module.vtbl != nullptr);
        mp::Demux demux;
        REQUIRE(demux.open(*module.vtbl, av_path()) == MP_OK);
        const Streams at = find(demux);
        REQUIRE(at.both);

        MpVideoInfo info{};
        REQUIRE(demux.video_info(at.video, info));
        // ffprobe reports this track's time_base as 1/24000, which is the same
        // fact from the other side: 24000 ticks a second.
        CHECK(info.timescale == 24000u);

        const std::uint32_t only_video[] = {at.video};
        REQUIRE(demux.select_streams(only_video) == MP_OK);

        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint64_t highest = 0;
        std::uint32_t packets = 0;
        std::uint32_t sync_packets = 0;
        bool reordered = false;
        std::uint64_t previous = 0;
        while (demux.read_packet(buffer, packet) == MP_OK) {
            REQUIRE((packet.flags & MP_PACKET_TIMED) != 0);
            // **Presentation order, arriving in storage order.** This file has
            // B-frames, so a packet whose timestamp is behind the one before it
            // is the file being correct rather than the demuxer being wrong --
            // and it is how the test knows a presentation timestamp is what
            // came back. A decode timestamp would climb.
            if (packets != 0 && packet.frame < previous) {
                reordered = true;
            }
            previous = packet.frame;
            highest = std::max(highest, packet.frame);
            if ((packet.flags & MP_PACKET_SYNC) != 0) {
                ++sync_packets;
            }
            ++packets;
        }
        CHECK(packets == 24);
        CHECK(reordered);

        // Not every video sample is one to start decoding from, which was
        // claimed unconditionally while only audio came through here.
        CHECK(sync_packets != 0);
        CHECK(sync_packets < packets);

        // **The number means a duration now, and this is the sum that says so
        // -- once the edit is taken off it.** A timestamp is container-relative
        // and `MpStreamInfo::skip_frames` is where the `elst` shift is stated,
        // exactly as it is for audio, so the host subtracts it in one place
        // rather than every demuxer folding it in differently. ffprobe folds it
        // in, which is why its numbers are 2002 ticks lower than these.
        MpStreamInfo video_stream{};
        REQUIRE(demux.stream_info(at.video, video_stream));
        REQUIRE(highest >= video_stream.skip_frames);
        const double seconds =
            static_cast<double>(highest - video_stream.skip_frames) / info.timescale;
        // 24 frames at 24000/1001, so the last one is presented at 23 of them.
        CHECK(seconds > 0.9);
        CHECK(seconds < 1.01);

        // And a seek is in the same units, which is what makes them a unit
        // rather than a decoration.
        //
        // **The bound is on the earliest of what comes back, not the first.**
        // A reordered stream arrives in storage order, so the packet after a
        // seek is not the earliest one the seek delivered -- and MP4 indexes
        // its seek by decode time while `frame` is a presentation time, so
        // landing exactly is a walk this module does not do yet. §9.9.
        const std::uint64_t target = video_stream.skip_frames + info.timescale / 2;
        REQUIRE(demux.seek(at.video, target) == MP_OK);
        std::uint64_t earliest = UINT64_MAX;
        for (int i = 0; i < 8 && demux.read_packet(buffer, packet) == MP_OK; ++i) {
            earliest = std::min(earliest, packet.frame);
        }
        REQUIRE(earliest != UINT64_MAX);

        // **At or before the target, which is the whole point.** The sample
        // table indexes decode time and `target` is a presentation time, so
        // the module moves the target back by the track's largest composition
        // offset before the lookup -- without that it landed exactly one frame
        // late on this file, which for video is the direction that cannot be
        // recovered.
        CHECK(earliest <= target);

        // And not absurdly early either: the sync-sample walk goes back to a
        // keyframe, and this file has one every six frames.
        const std::uint64_t one_frame =
            static_cast<std::uint64_t>(info.timescale) * info.fps_den / info.fps_num;
        REQUIRE(one_frame != 0);
        CHECK(earliest + 8 * one_frame > target);
    }

    SECTION("Matroska counts in nanoseconds, because that is what it hands back")
    {
        Module module{mkv_module()};
        REQUIRE(module.vtbl != nullptr);
        mp::Demux demux;
        REQUIRE(demux.open(*module.vtbl, mkv_path()) == MP_OK);
        const Streams at = find(demux);
        REQUIRE(at.both);

        MpVideoInfo info{};
        REQUIRE(demux.video_info(at.video, info));
        CHECK(info.timescale == 1000000000u);
        CHECK(info.width == 128);
        CHECK(info.height == 96);
        // Not stated separately, so the pixels are square and the coded size is
        // the display size.
        CHECK(info.display_width == 128);
        CHECK(info.display_height == 96);

        // **Matroska states a duration per frame rather than a rate**, so the
        // ratio is the reciprocal of a rounded nanosecond count: 24000/1001 was
        // written as 41708333 ns and does not come back out as 24000/1001. It
        // is what the container says, which is the only thing a demuxer may
        // report.
        REQUIRE(info.fps_den != 0);
        const double fps = static_cast<double>(info.fps_num) / info.fps_den;
        CHECK(fps > 23.9);
        CHECK(fps < 24.0);

        const std::uint32_t only_video[] = {at.video};
        REQUIRE(demux.select_streams(only_video) == MP_OK);
        std::vector<std::uint8_t> buffer;
        MpPacket packet{};
        std::uint64_t highest = 0;
        std::uint64_t previous = 0;
        std::uint32_t packets = 0;
        bool reordered = false;
        while (demux.read_packet(buffer, packet) == MP_OK) {
            if ((packet.flags & MP_PACKET_TIMED) == 0) {
                continue;
            }
            if (packets != 0 && packet.frame < previous) {
                reordered = true;
            }
            previous = packet.frame;
            highest = std::max(highest, packet.frame);
            ++packets;
        }
        // The same reordering as the MP4, which is what makes the two
        // containers agree about what the field means rather than merely both
        // filling it in.
        CHECK(reordered);
        const double seconds = static_cast<double>(highest) / info.timescale;
        CHECK(seconds > 0.9);
        CHECK(seconds < 1.01);

        // Half a second, in the nanoseconds this stream counts in. Same
        // bound as the MP4 and for the same reason: the earliest of what came
        // back, because storage order is not presentation order.
        REQUIRE(demux.seek(at.video, 500000000ull) == MP_OK);
        std::uint64_t earliest = UINT64_MAX;
        for (int i = 0; i < 8 && demux.read_packet(buffer, packet) == MP_OK; ++i) {
            earliest = std::min(earliest, packet.frame);
        }
        REQUIRE(earliest != UINT64_MAX);
        CHECK(earliest <= 500000000ull);
    }

    SECTION("an older host asks for a smaller struct and gets one")
    {
        // MpVideoInfo grew once, for exactly this field. A module that wrote
        // the whole struct into a buffer sized by the older header would be
        // scribbling past it -- which is the failure a size-prefixed struct
        // exists to prevent and the one nothing would otherwise have caught.
        // Driven through the vtable rather than through mp::Demux, because
        // mp::Demux always asks for the size it was compiled with.
        Module module{mp4_module()};
        REQUIRE(module.vtbl != nullptr);
        REQUIRE(module.vtbl->stream_video_info != nullptr);

        MpDemux* handle = nullptr;
        REQUIRE(module.vtbl->open(av_path(), &handle) == MP_OK);
        REQUIRE(handle != nullptr);

        struct Guarded {
            MpVideoInfo info;
            std::uint32_t canary;
        };
        Guarded guarded{};
        guarded.canary = 0xFEEDFACEu;
        // The size the header had before `timescale` was appended.
        guarded.info.size = sizeof(MpVideoInfo) - sizeof(std::uint32_t);

        // Stream 0 is the video track in this file, which the sections above
        // establish; asking the vtable directly means saying so here.
        REQUIRE(module.vtbl->stream_video_info(handle, 0, &guarded.info) == MP_OK);
        CHECK(guarded.info.width == 128);
        // Not written, because the caller said its struct stops before it.
        CHECK(guarded.info.timescale == 0u);
        CHECK(guarded.canary == 0xFEEDFACEu);

        module.vtbl->close(handle);
    }
}

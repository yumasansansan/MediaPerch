// SPDX-License-Identifier: GPL-3.0-or-later
//
// A container and a codec, joined, with neither of them real.
//
// No module implements the v2 interfaces yet, which is exactly why this exists:
// the plumbing between a demuxer and a codec is where the migration's hard
// parts live -- the gapless edit, growing a packet buffer, and seeking, which
// v1 hid inside each decoder and each decoder therefore had to be right about
// separately. Fake vtables let all three be checked before the first real
// demuxer is written, so the first one has something to be wrong against.

#include "mediaperch/packet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

mp::Format cd_audio()
{
    return mp::Format{.sample_rate = 44100,
                      .channels = 2,
                      .channel_mask = 0,
                      .sample_type = mp::SampleType::s16,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = 0};
}

/// One frame is four bytes, and every frame is its own index, so a shift of one
/// frame is visible rather than plausible.
std::vector<std::uint8_t> frames(std::uint32_t first, std::uint32_t count)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(count) * 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t value = first + i;
        std::memcpy(out.data() + static_cast<std::size_t>(i) * 4, &value, 4);
    }
    return out;
}

/// What the fake container holds, as file-scope state: the fakes have no
/// instance of their own and a test is one thread.
struct World {
    std::uint32_t frames_per_packet = 16;
    std::uint32_t packets = 8;
    std::uint32_t next_packet = 0;
    /// Bytes each packet claims to be. Larger than the host's starting buffer
    /// in one test, to make it grow.
    std::uint32_t packet_bytes = 8;
    std::uint64_t skip = 0;
    std::uint64_t play = 0;
    std::uint64_t trim = 0;
    bool self_decodes = false;
    std::uint32_t stream_count = 1;
    /// How many packets before the target a seek lands on, as a codec that
    /// needs pre-roll would ask for.
    std::uint32_t preroll = 0;
    /// A demuxer that will not say where its packets are.
    bool untimed = false;
    /// What the codec has been told, so the test can see the seek handshake.
    int resets = 0;
    std::uint32_t decoded_packets = 0;
};

World g;

// --- the demuxer -----------------------------------------------------------

MpResult MP_CALL demux_open(const char*, MpDemux** out)
{
    g.next_packet = 0;
    *out = reinterpret_cast<MpDemux*>(&g);
    return MP_OK;
}
void MP_CALL demux_close(MpDemux*) {}

MpResult MP_CALL demux_stream_count(MpDemux*, std::uint32_t* out)
{
    *out = g.stream_count;
    return MP_OK;
}

MpResult MP_CALL demux_stream_info(MpDemux*, std::uint32_t index, MpStreamInfo* out)
{
    if (index >= g.stream_count) {
        return MP_ERR_INVALID;
    }
    out->index = index;
    // Stream 0 is video when there is more than one, so "the first stream" and
    // "the audio stream" are different answers and the test can tell them apart.
    const bool audio = g.stream_count == 1 || index == 1;
    out->kind = audio ? MP_STREAM_AUDIO : MP_STREAM_VIDEO;
    out->codec = audio ? (g.self_decodes ? MP_CODEC_INTERNAL : MP_CODEC_FLAC)
                       : MP_CODEC_UNKNOWN;
    out->flags = g.self_decodes && audio ? MP_STREAM_SELF_DECODES : 0u;
    out->config_bytes = 4;
    out->format = mp::to_abi(cd_audio());
    out->total_frames = static_cast<std::uint64_t>(g.frames_per_packet) * g.packets;
    out->skip_frames = g.skip;
    out->play_frames = g.play;
    out->trim_frames = g.trim;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux*, std::uint32_t, std::uint8_t* out,
                                     std::uint32_t out_bytes, std::uint32_t* needed)
{
    *needed = 4;
    if (out != nullptr && out_bytes >= 4) {
        std::memcpy(out, "cfg!", 4);
    }
    return MP_OK;
}

MpResult MP_CALL demux_select(MpDemux*, std::uint32_t) { return MP_OK; }

MpResult MP_CALL demux_read_packet(MpDemux*, void* dst, std::size_t dst_bytes,
                                   MpPacket* out)
{
    if (g.next_packet >= g.packets) {
        out->bytes = 0;
        return MP_END;
    }
    if (dst_bytes < g.packet_bytes) {
        // **Nothing is consumed**, and what it needs is reported. The host is
        // expected to grow and ask again.
        out->bytes = g.packet_bytes;
        return MP_ERR_NO_MEMORY;
    }
    // The packet's payload is the index of its first frame, which is all the
    // fake codec needs to produce the right samples.
    const std::uint32_t first = g.next_packet * g.frames_per_packet;
    std::memset(dst, 0, g.packet_bytes);
    std::memcpy(dst, &first, 4);
    out->bytes = g.packet_bytes;
    out->frame = first;
    // This fake knows where every packet is, so it says so. A demuxer that did
    // not would leave the host unable to discard what precedes a seek target.
    out->flags = g.untimed ? 0u : MP_PACKET_TIMED;
    ++g.next_packet;
    return MP_OK;
}

MpResult MP_CALL demux_seek(MpDemux*, std::uint64_t frame)
{
    // To the packet containing the frame, and `g.preroll` packets before that
    // when the test is standing in for a codec that needs warming. Both leave
    // audio in front of the target for the host to discard.
    const std::uint32_t index = static_cast<std::uint32_t>(frame / g.frames_per_packet);
    g.next_packet = index > g.preroll ? index - g.preroll : 0;
    return MP_OK;
}

MpResult MP_CALL demux_read_frames(MpDemux*, void* dst, std::size_t dst_bytes,
                                   std::size_t* out_bytes)
{
    *out_bytes = 0;
    if (g.next_packet >= g.packets) {
        return MP_END;
    }
    const auto pcm = frames(g.next_packet * g.frames_per_packet, g.frames_per_packet);
    if (dst_bytes < pcm.size()) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(dst, pcm.data(), pcm.size());
    *out_bytes = pcm.size();
    ++g.next_packet;
    return MP_OK;
}

const MpDemuxVtbl& demux_vtbl()
{
    static const MpDemuxVtbl vtbl{sizeof(MpDemuxVtbl),
                                  0,
                                  nullptr, /* probe: the host picked it already */
                                  &demux_open,
                                  &demux_stream_count,
                                  &demux_stream_info,
                                  &demux_stream_config,
                                  &demux_select,
                                  &demux_read_packet,
                                  &demux_seek,
                                  &demux_read_frames,
                                  &demux_close};
    return vtbl;
}

// --- the codec -------------------------------------------------------------

MpResult MP_CALL codec_open(MpCodec codec, const std::uint8_t* config,
                            std::uint32_t config_bytes, MpCodecInstance** out)
{
    if (codec != MP_CODEC_FLAC) {
        return MP_ERR_UNSUPPORTED;
    }
    // The container's blob, verbatim and checked: a codec that is handed
    // somebody else's configuration decodes noise, and the check is one line.
    if (config == nullptr || config_bytes != 4 || std::memcmp(config, "cfg!", 4) != 0) {
        return MP_ERR_FORMAT;
    }
    g.decoded_packets = 0;
    *out = reinterpret_cast<MpCodecInstance*>(&g);
    return MP_OK;
}
void MP_CALL codec_close(MpCodecInstance*) {}

MpResult MP_CALL codec_get_format(MpCodecInstance*, MpFormat* out)
{
    *out = mp::to_abi(cd_audio());
    return MP_OK;
}

MpResult MP_CALL codec_decode(MpCodecInstance*, const void* packet, std::size_t bytes,
                              void* dst, std::size_t dst_bytes, std::size_t* out_bytes)
{
    *out_bytes = 0;
    if (packet == nullptr || bytes < 4) {
        return MP_ERR_INVALID;
    }
    std::uint32_t first = 0;
    std::memcpy(&first, packet, 4);
    const auto pcm = frames(first, g.frames_per_packet);
    if (dst_bytes < pcm.size()) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(dst, pcm.data(), pcm.size());
    *out_bytes = pcm.size();
    ++g.decoded_packets;
    return MP_OK;
}

MpResult MP_CALL codec_flush(MpCodecInstance*, void*, std::size_t, std::size_t* out_bytes)
{
    *out_bytes = 0;
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance*)
{
    ++g.resets;
    return MP_OK;
}

const MpCodecVtbl& codec_vtbl()
{
    static const MpCodecVtbl vtbl{sizeof(MpCodecVtbl), 0,
                                  nullptr, /* probe */
                                  &codec_open,        &codec_get_format, &codec_decode,
                                  &codec_flush,       &codec_reset,      &codec_close};
    return vtbl;
}

mp::PacketSource::FindCodec finds_flac()
{
    return [](MpCodec codec, const std::uint8_t* config,
              std::uint32_t config_bytes) -> const MpCodecVtbl* {
        // The blob reaches the lookup, which is how a codec declines a stream
        // it cannot take rather than failing to open one it could have.
        if (codec != MP_CODEC_FLAC || config == nullptr || config_bytes != 4) {
            return nullptr;
        }
        return &codec_vtbl();
    };
}

/// Reads the whole source, in awkwardly sized bites.
std::vector<std::uint8_t> drain(mp::ISource& source, std::size_t bite = 30)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> chunk(bite);
    for (;;) {
        const std::size_t got = source.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

} // namespace

TEST_CASE("a container and a codec become one source", "[packet]")
{
    g = World{};
    mp::PacketSource source;
    std::string why;
    INFO(why);
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    CHECK(source.format() == cd_audio());
    CHECK(source.length_frames() == 128);
    CHECK_FALSE(source.self_decoded());

    // Every frame, in order, once. The packets were reassembled and nothing
    // was dropped between the two modules.
    const auto out = drain(source);
    REQUIRE(out == frames(0, 128));
}

TEST_CASE("the audio stream is chosen, not the first one", "[packet]")
{
    // A file is not one stream. This container's stream 0 is video.
    g = World{};
    g.stream_count = 2;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));
    CHECK(source.stream().kind == MP_STREAM_AUDIO);
    CHECK(source.stream().index == 1);
}

TEST_CASE("a codec nobody has is a different sentence from a file nobody reads",
          "[packet]")
{
    // The container opened and described itself perfectly. What is missing is a
    // decoder for what is in it, and saying so is the whole point of looking
    // the codec up instead of trying decoders until one works.
    g = World{};
    mp::PacketSource source;
    std::string why;
    CHECK_FALSE(source.open(
        demux_vtbl(), "x",
        [](MpCodec, const std::uint8_t*, std::uint32_t) -> const MpCodecVtbl* {
            return nullptr;
        },
        why));
    CHECK(why.find("decodes that codec") != std::string::npos);
}

TEST_CASE("a packet that does not fit is not lost", "[packet]")
{
    // The demuxer says what it needs and consumes nothing; the host grows and
    // asks again. A host that dropped the packet would lose audio at whatever
    // bitrate first exceeded its buffer, which is the kind of bug that only
    // appears on somebody else's files.
    g = World{};
    g.packet_bytes = 64 * 1024; // past the host's starting buffer
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));
    const auto out = drain(source);
    CHECK(out == frames(0, 128));
    CHECK(g.decoded_packets == 8);
}

TEST_CASE("the gapless edit is the container's, and it is applied once", "[packet]")
{
    // `skip_frames` and `play_frames` came from three separate decoders in v1
    // and each had to be right about them alone. Here they arrive in
    // MpStreamInfo and are applied in one place.
    g = World{};
    g.skip = 5;
    g.play = 100;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    CHECK(source.length_frames() == 100);
    const auto out = drain(source);
    // The encoder's warm-up is gone from the front and the file's own length
    // stops it: frames 5 through 104.
    REQUIRE(out == frames(5, 100));
}

TEST_CASE("a tail trim drops the end without shortening anything else", "[packet]")
{
    // **A trim is not a length**, which is why it has a field of its own.
    // Matroska states one and cannot state the other: its timestamps are scaled
    // to the millisecond, so a length taken from them is rounded, and rounding a
    // lossless track's length truncates it. See MpStreamInfo::trim_frames.
    //
    // 8 packets of 16 frames is 128, and the last 7 are the encoder's padding.
    g = World{};
    g.trim = 7;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    const auto out = drain(source);
    REQUIRE(out == frames(0, 121));
    // Every packet was still decoded: the trim holds frames back, it does not
    // stop the demuxer early.
    CHECK(g.decoded_packets == 8);
}

TEST_CASE("a trim and a head skip do not disturb each other", "[packet]")
{
    g = World{};
    g.skip = 5;
    g.trim = 7;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    // Frames 5 through 120: the warm-up gone from the front, the padding from
    // the back, and nothing in between touched.
    REQUIRE(drain(source) == frames(5, 116));
}

TEST_CASE("a trim longer than the buffer still leaves the front intact", "[packet]")
{
    // The held-back frames outlast a single packet, so the carry has to survive
    // being refilled -- which is the part that would silently drop audio.
    g = World{};
    g.trim = 40;  // two and a half packets
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(drain(source) == frames(0, 88));
}

TEST_CASE("a trim nobody could mean is ignored rather than obeyed", "[packet]")
{
    // The number came out of a file. An uncapped one would decide how much the
    // host holds in memory, so anything past a second of audio is nonsense and
    // is dropped rather than trusted.
    g = World{};
    g.trim = 1u << 30;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(drain(source) == frames(0, 128));
}

TEST_CASE("a seek resets the codec and lands on the right sample", "[packet]")
{
    g = World{};
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));
    REQUIRE(source.seekable());

    const int before = g.resets;
    REQUIRE(source.seek(32));
    // The codec was told to forget. v1 hid this inside each decoder, which is
    // why each one had to be right about it separately.
    CHECK(g.resets == before + 1);

    const auto out = drain(source);
    REQUIRE(out == frames(32, 96));
}

TEST_CASE("a seek keeps the tail trim and drops what was held back", "[packet]")
{
    // The held-back frames are audio from *before* the seek. Carrying them
    // across would splice the old position onto the new one, quietly, at the
    // first read -- so the seek clears them and the trim goes on applying.
    g = World{};
    g.trim = 7;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    // Read enough to put frames in the holdback, then leave.
    std::vector<std::uint8_t> scratch(64);
    REQUIRE(source.read(scratch.data(), scratch.size()) == 64);
    REQUIRE(source.seek(32));

    // Frames 32 through 120: the seek target at the front, the padding still
    // gone from the back, and nothing from before the seek in between.
    REQUIRE(drain(source) == frames(32, 89));
}

TEST_CASE("a seek lands on the frame asked for, not on the packet holding it",
          "[packet]")
{
    // **A demuxer seeks to a packet.** Sixteen frames to a packet here, so a
    // seek to 37 lands on the packet that starts at 32 and five frames of audio
    // arrive that the caller did not ask for. Discarding them is the host's,
    // because a demuxer cannot: it does not decode, so it cannot count frames
    // out of a packet it only located.
    g = World{};
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(source.seek(37));
    const auto out = drain(source);
    REQUIRE(out == frames(37, 91));
}

TEST_CASE("a codec's pre-roll is decoded and then thrown away", "[packet]")
{
    // AAC's overlap and MP3's bit reservoir mean the packet containing a frame
    // is not enough to decode it correctly: the demuxer deliberately hands back
    // earlier packets as well. They are decoded -- that is the point, it is what
    // warms the codec -- and then discarded by the same count that a mid-packet
    // seek uses, because it is the same question.
    g = World{};
    g.preroll = 2;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(source.seek(64));
    const auto out = drain(source);
    REQUIRE(out == frames(64, 64));
}

TEST_CASE("a demuxer that will not vouch for a packet's position is believed",
          "[packet]")
{
    // MP_PACKET_TIMED is a demuxer saying `frame` is real. Without it, zero is
    // both "the start of the stream" and "I do not timestamp", and a host that
    // discarded on the difference would silently swallow the first seconds of
    // every file from such a module. It does not discard instead, which is the
    // old behaviour and the safe one.
    g = World{};
    g.untimed = true;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(source.seek(37));
    const auto out = drain(source);
    // The packet holding frame 37 starts at 32, and nothing trimmed it.
    REQUIRE(out == frames(32, 96));
}

TEST_CASE("a seek inside an edited stream is measured from the edit", "[packet]")
{
    // The caller counts from the first audible frame; the container counts from
    // the first stored one. Confusing the two puts every seek in an edited file
    // off by the encoder's warm-up.
    g = World{};
    g.skip = 16;
    g.play = 96;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x", finds_flac(), why));

    REQUIRE(source.seek(16));
    const auto out = drain(source);
    // Frame 16 of the *audio* is frame 32 of the stream.
    REQUIRE(out == frames(32, 80));
}

TEST_CASE("a demuxer that decodes for itself is asked to", "[packet]")
{
    // Media Foundation and FFmpeg are pipelines, not container readers that
    // happen to decode. The flag says so and no codec is looked up.
    g = World{};
    g.self_decodes = true;
    mp::PacketSource source;
    std::string why;
    REQUIRE(source.open(demux_vtbl(), "x",
                        [](MpCodec, const std::uint8_t*, std::uint32_t)
                            -> const MpCodecVtbl* {
                            FAIL("a self-decoding stream must not look up a codec");
                            return nullptr;
                        },
                        why));
    CHECK(source.self_decoded());
    const auto out = drain(source);
    REQUIRE(out == frames(0, 128));
}

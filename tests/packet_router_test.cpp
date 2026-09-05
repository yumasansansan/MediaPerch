// SPDX-License-Identifier: GPL-3.0-or-later
//
// One demuxer, two consumers, and the position they share.
//
// **§4 says one file has one position**, and the ABI header says what is wrong
// with the alternative in as many words: opening the container twice means two
// positions, and a seek then has to move both and land them on the same moment.
// So a player reading audio and video out of one file reads it once, and
// something routes what comes out. This is that something, and the test it
// wants is the one below: **the packets each stream gets through the router are
// the packets it would have got from a demuxer of its own** -- same bytes, same
// order, same count -- while only one file position exists.

#include "mediaperch/packet.hpp"

#include "module_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using mp::test::Module;

namespace {

/// One packet, kept so that two readings of the same file can be compared.
struct Seen {
    std::uint64_t frame = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> bytes;

    bool operator==(const Seen& other) const noexcept
    {
        return frame == other.frame && flags == other.flags && bytes == other.bytes;
    }
};

/// The two streams of the fixture, found rather than assumed.
struct Streams {
    std::uint32_t audio = 0;
    std::uint32_t video = 0;
    bool both = false;
};

Streams find_streams(mp::Demux& demux)
{
    Streams out;
    bool audio = false;
    bool video = false;
    MpStreamInfo info{};
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
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

/// Everything one stream carries, read by a demuxer that reads nothing else.
/// The reference the router is held to.
std::vector<Seen> alone(const MpDemuxVtbl& vtbl, std::uint32_t stream)
{
    mp::Demux demux;
    REQUIRE(demux.open(vtbl, MEDIAPERCH_TEST_AV) == MP_OK);
    REQUIRE(demux.select(stream) == MP_OK);

    std::vector<Seen> out;
    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    while (demux.read_packet(buffer, packet) == MP_OK) {
        Seen seen;
        seen.frame = packet.frame;
        seen.flags = packet.flags;
        seen.bytes.assign(buffer.begin(), buffer.begin() + packet.bytes);
        out.push_back(std::move(seen));
    }
    return out;
}

/// One packet out of a feed, or nothing.
enum class Got { packet, end, busy };

Got take(mp::IPacketFeed& feed, std::vector<Seen>& into)
{
    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    const MpResult r = feed.next(buffer, packet);
    if (r == MP_END) {
        return Got::end;
    }
    if (r == MP_ERR_BUSY) {
        return Got::busy;
    }
    REQUIRE(r == MP_OK);
    Seen seen;
    seen.frame = packet.frame;
    seen.flags = packet.flags;
    seen.bytes.assign(buffer.begin(), buffer.begin() + packet.bytes);
    into.push_back(std::move(seen));
    return Got::packet;
}

} // namespace

TEST_CASE("one demuxer serves both streams the packets two would have",
          "[packet][router]")
{
    Module module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    REQUIRE(module.as<MpDemuxVtbl>() != nullptr);
    const MpDemuxVtbl& vtbl = *module.as<MpDemuxVtbl>();

    mp::Demux demux;
    REQUIRE(demux.open(vtbl, MEDIAPERCH_TEST_AV) == MP_OK);
    const Streams at = find_streams(demux);
    REQUIRE(at.both);

    // The reference: what each stream is, read on its own. Two file positions,
    // which is exactly what the router exists to avoid needing.
    const std::vector<Seen> audio_alone = alone(vtbl, at.audio);
    const std::vector<Seen> video_alone = alone(vtbl, at.video);
    REQUIRE(!audio_alone.empty());
    REQUIRE(!video_alone.empty());

    const std::uint32_t both[] = {at.audio, at.video};
    REQUIRE(demux.select_streams(both) == MP_OK);
    mp::PacketRouter router{demux, both};

    mp::IPacketFeed* audio = router.feed(at.audio);
    mp::IPacketFeed* video = router.feed(at.video);
    REQUIRE(audio != nullptr);
    REQUIRE(video != nullptr);
    // A stream this router was not given has no feed, rather than an empty one.
    CHECK(router.feed(999) == nullptr);

    SECTION("asked in turn, which is how a player asks")
    {
        std::vector<Seen> got_audio;
        std::vector<Seen> got_video;
        bool audio_done = false;
        bool video_done = false;
        while (!audio_done || !video_done) {
            if (!audio_done && take(*audio, got_audio) == Got::end) {
                audio_done = true;
            }
            if (!video_done && take(*video, got_video) == Got::end) {
                video_done = true;
            }
        }
        CHECK(got_audio == audio_alone);
        CHECK(got_video == video_alone);

        // Every packet was read once. A router that read the file twice, or
        // kept a packet nobody asked for, would fail here rather than leak.
        const mp::PacketRouter::Stats stats = router.stats();
        CHECK(stats.read == audio_alone.size() + video_alone.size());
        CHECK(stats.queued_packets == 0);
        CHECK(stats.queued_bytes == 0);
        // And most packets went straight into the caller's own buffer, because
        // the file interleaves: only the ones read while the other consumer was
        // asking had to wait anywhere.
        CHECK(stats.queued < stats.read);
    }

    SECTION("one stream drained to the end first, which is the hard case")
    {
        // The whole video track pulled through before the audio is asked for
        // anything. Every audio packet in the file has to wait somewhere, and
        // the cap is what says how many may.
        std::vector<Seen> got_video;
        while (take(*video, got_video) != Got::end) {
        }
        CHECK(got_video == video_alone);
        CHECK(router.stats().queued_packets == audio_alone.size());

        std::vector<Seen> got_audio;
        while (take(*audio, got_audio) != Got::end) {
        }
        CHECK(got_audio == audio_alone);
        CHECK(router.stats().queued_packets == 0);
    }
}

TEST_CASE("a queue that fills says so rather than growing", "[packet][router]")
{
    // A file whose interleave is not sane, or a consumer that stopped asking,
    // would otherwise be memory exhaustion. Dropping the packets instead would
    // be silent corruption and blocking would be a deadlock, so the answer is a
    // result code the caller can act on.
    Module module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    REQUIRE(module.as<MpDemuxVtbl>() != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV) == MP_OK);
    const Streams at = find_streams(demux);
    REQUIRE(at.both);
    const std::uint32_t both[] = {at.audio, at.video};
    REQUIRE(demux.select_streams(both) == MP_OK);

    // **Five hundred and twelve bytes, because the whole audio track is four
    // thousand.** Measured: forty-four packets averaging ninety-four bytes, a
    // second of AAC at a low rate. A cap of four kilobytes would have been the
    // entire track, and the queue would only have filled after the file ran
    // out -- which tests the end of the file rather than the cap.
    mp::PacketRouter::Limits tight;
    tight.queued_bytes_per_stream = 512;
    mp::PacketRouter router{demux, both, tight};
    mp::IPacketFeed* audio = router.feed(at.audio);
    mp::IPacketFeed* video = router.feed(at.video);
    REQUIRE(audio != nullptr);
    REQUIRE(video != nullptr);

    std::vector<Seen> got_video;
    Got last = Got::packet;
    for (int guard = 0; guard < 10000 && last == Got::packet; ++guard) {
        last = take(*video, got_video);
    }
    // It stopped, and it stopped by saying so.
    CHECK(last == Got::busy);
    CHECK(!got_video.empty());
    // Part way through the file, which is what makes this the cap rather than
    // the end of the stream.
    CHECK(got_video.size() < 24);
    // Over the cap by at most one packet: the one that discovered it, because
    // a packet already read cannot be put back.
    const std::size_t over = router.stats().queued_bytes;
    CHECK(over >= tight.queued_bytes_per_stream);
    CHECK(over < tight.queued_bytes_per_stream + 4096);

    // Draining the other side is what unblocks it, which is what MP_ERR_BUSY
    // was telling the caller to do. One packet is enough, because the cap was
    // exceeded by less than one packet's worth.
    std::vector<Seen> got_audio;
    CHECK(take(*audio, got_audio) == Got::packet);
    CHECK(take(*video, got_video) == Got::packet);

    // And with the other side kept drained, the rest of the file comes out.
    for (int guard = 0; guard < 10000 && got_video.size() < 24; ++guard) {
        if (take(*video, got_video) == Got::busy) {
            take(*audio, got_audio);
        }
    }
    CHECK(got_video.size() == 24);
}

TEST_CASE("a seek moves the file and empties what was read before it",
          "[packet][router]")
{
    // The half that cannot be separated from the other: a seek that left the
    // queues alone would hand a consumer packets from before it, which is the
    // one thing a seek is for.
    Module module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    REQUIRE(module.as<MpDemuxVtbl>() != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV) == MP_OK);
    const Streams at = find_streams(demux);
    REQUIRE(at.both);
    const std::uint32_t both[] = {at.audio, at.video};
    REQUIRE(demux.select_streams(both) == MP_OK);

    const std::vector<Seen> video_alone = alone(*module.as<MpDemuxVtbl>(), at.video);
    REQUIRE(video_alone.size() > 8);

    mp::PacketRouter router{demux, both};
    mp::IPacketFeed* video = router.feed(at.video);
    mp::IPacketFeed* audio = router.feed(at.audio);
    REQUIRE(video != nullptr);
    REQUIRE(audio != nullptr);

    // Read some video, which leaves audio waiting.
    std::vector<Seen> got_video;
    for (int i = 0; i < 8; ++i) {
        REQUIRE(take(*video, got_video) == Got::packet);
    }
    REQUIRE(router.stats().queued_packets > 0);

    MpStreamInfo info{};
    REQUIRE(demux.stream_info(at.audio, info));
    REQUIRE(info.format.sample_rate != 0);
    REQUIRE(router.seek(at.audio, info.format.sample_rate / 2) == MP_OK);
    CHECK(router.stats().queued_packets == 0);
    CHECK(router.stats().queued_bytes == 0);

    // **What comes out next is from where the file now is, not from where the
    // reading had got to.** A router that kept its queues would have handed
    // over the ninth video packet, which is the one thing a seek is against.
    std::vector<Seen> after;
    REQUIRE(take(*video, after) == Got::packet);
    const auto at_index =
        std::find(video_alone.begin(), video_alone.end(), after.front());
    REQUIRE(at_index != video_alone.end());
    const std::size_t landed = static_cast<std::size_t>(at_index - video_alone.begin());
    CHECK(landed < 8);

    // Which is §9.9's rule rather than this file being short: a seek lands at
    // or before the target, because the target is a presentation time and the
    // sample table indexes decode time. This fixture's only sync sample is its
    // first frame, so it lands there.
    CHECK(landed == 0);

    // Both feeds still work afterwards, which is the other half of one file
    // having one position: the seek moved the audio too.
    std::vector<Seen> got_audio;
    CHECK(take(*audio, got_audio) == Got::packet);
}

// --------------------------------------------------------------------------
// And the audio side, reading through it
// --------------------------------------------------------------------------

#if defined(MEDIAPERCH_CODEC_OPUS) && defined(MEDIAPERCH_DEMUX_MKV) && \
    defined(MEDIAPERCH_TEST_AV_MKV)

namespace {

/// Everything a source hands out, in awkwardly sized bites so that the answer
/// cannot depend on how it was asked for.
std::vector<std::uint8_t> drain(mp::ISource& source, std::size_t bite = 997)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> chunk(bite);
    for (;;) {
        const std::size_t got = source.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

} // namespace

TEST_CASE("a source reading through the router decodes what one with its own "
          "demuxer does",
          "[packet][router]")
{
    // **The equivalence again, one layer up.** The packets were already shown
    // to be the same; this says the whole of `PacketSource` -- the gapless
    // edit, the trim, the packet buffer that grows -- is the same code doing
    // the same thing, and that only where the packets come from changed.
    Module demux_module{MEDIAPERCH_DEMUX_MKV, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_OPUS, MP_KIND_CODEC};
    REQUIRE(demux_module.as<MpDemuxVtbl>() != nullptr);
    REQUIRE(codec_module.as<MpCodecVtbl>() != nullptr);

    const MpCodecVtbl* codec_vtbl = codec_module.as<MpCodecVtbl>();
    const mp::PacketSource::FindCodec find =
        [codec_vtbl](MpCodec codec, const std::uint8_t*, std::uint32_t) {
            return codec == MP_CODEC_OPUS ? codec_vtbl : nullptr;
        };

    // Its own demuxer, which is what every audio-only run does today.
    std::vector<std::uint8_t> alone_pcm;
    mp::Format alone_format{};
    {
        mp::PacketSource source;
        std::string why;
        INFO(why);
        REQUIRE(source.open(*demux_module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV_MKV, find,
                            why));
        alone_format = source.format();
        alone_pcm = drain(source);
    }
    REQUIRE(!alone_pcm.empty());

    // The same audio, out of a demuxer that video is reading too.
    mp::Demux demux;
    REQUIRE(demux.open(*demux_module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV_MKV) == MP_OK);
    const Streams at = find_streams(demux);
    REQUIRE(at.both);
    const std::uint32_t both[] = {at.audio, at.video};
    REQUIRE(demux.select_streams(both) == MP_OK);
    mp::PacketRouter router{demux, both};

    mp::PacketSource routed;
    std::string why;
    INFO(why);
    REQUIRE(routed.open(router, at.audio, find, why));
    CHECK(routed.format() == alone_format);

    // The video side is not asking, so every video packet in the file waits --
    // which is what the cap is for and, at its default, what this file fits
    // inside easily.
    const std::vector<std::uint8_t> routed_pcm = drain(routed);
    CHECK(routed_pcm == alone_pcm);
    CHECK(router.stats().queued_packets > 0);

    // And a stream the router was not given is refused rather than opened on
    // the demuxer behind its back.
    mp::PacketSource stray;
    std::string stray_why;
    CHECK_FALSE(stray.open(router, 999, find, stray_why));
    CHECK(!stray_why.empty());
}

#endif

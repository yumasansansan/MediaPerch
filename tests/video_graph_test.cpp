// SPDX-License-Identifier: GPL-3.0-or-later
//
// §8 through the real modules: a container, a decoder, a presenter, and a clock
// somebody chose.
//
// **This is M6's claim in miniature.** The milestone asks that video plays with
// frames dropped against audio and never the reverse, and the part of that
// which does not need a 4K file and a display is exactly this: a real MP4, a
// real AV1 decoder, the real presenter, and an audio clock that is a number.
// What the fixture is small enough to leave out is the *load* -- a machine that
// cannot decode 4K in real time -- and that is arranged here instead by moving
// the clock, which produces the same decision for the same reason.
//
// `avsync_test.cpp` checks the arithmetic. This checks that the arithmetic is
// wired to a decoder that reorders B-frames and a presenter that draws pixels.

#include "mediaperch/video.hpp"

#include "mediaperch/avsync.hpp"
#include "mediaperch/packet.hpp"
#include "module_loader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using mp::test::Module;

namespace {

/// One demuxer's selected stream, as a feed.
///
/// Four lines, which is what `IPacketFeed` promised: a caller reading a single
/// stream needs no router. The router is for the day audio and video come out
/// of one demuxer, which is what §4 means by one file having one position.
class OneStream final : public mp::IPacketFeed {
public:
    explicit OneStream(mp::Demux& demux) : demux_(&demux) {}
    MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) override
    {
        return demux_->read_packet(buffer, out);
    }

private:
    mp::Demux* demux_;
};

/// Everything a run needs, opened once.
struct Chain {
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};

    mp::Demux demux;
    mp::VideoDecoder decoder;
    mp::Presenter presenter;
    MpVideoInfo info{};

    Chain()
    {
        REQUIRE(demux_module.as<MpDemuxVtbl>() != nullptr);
        REQUIRE(codec_module.as<MpVideoCodecVtbl>() != nullptr);
        REQUIRE(video_module.as<MpVideoVtbl>() != nullptr);

        REQUIRE(demux.open(*demux_module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV1) == MP_OK);

        std::uint32_t stream = 0;
        MpStreamInfo stream_info{};
        bool found = false;
        for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
            if (demux.stream_info(i, stream_info) && stream_info.kind == MP_STREAM_VIDEO) {
                stream = i;
                found = true;
                break;
            }
        }
        REQUIRE(found);

        info.size = sizeof(info);
        REQUIRE(demux.video_info(stream, info));

        // WARP, because a test that depended on which GPU is in the machine
        // would be a test of the machine. video_d3d11_test.cpp makes the same
        // choice for the same reason.
        REQUIRE(presenter.open(*video_module.as<MpVideoVtbl>(), nullptr) == MP_OK);
        REQUIRE(presenter.set("device", "warp") == MP_OK);
        REQUIRE(presenter.configure(info) == MP_OK);

        // §9.8.1: the decoder is handed the presenter's device rather than
        // making one, so that a hardware decoder's textures are ones this
        // presenter can sample. dav1d is CPU and ignores it, which is what
        // makes it a safe first caller of the handshake.
        MpGraphicsDevice device{};
        REQUIRE(presenter.get_device(device) == MP_OK);

        std::vector<std::uint8_t> config;
        (void)demux.stream_config(stream, config);
        REQUIRE(decoder.open(*codec_module.as<MpVideoCodecVtbl>(), MP_CODEC_AV1, &device,
                             config.data(),
                             static_cast<std::uint32_t>(config.size())) == MP_OK);

        const std::uint32_t only_video[] = {stream};
        REQUIRE(demux.select_streams(only_video) == MP_OK);
    }
};

/// A display loop with time made of arithmetic: pump, and when nothing was due,
/// come back a millisecond later.
struct Played {
    double finished_at = 0.0;
    std::uint64_t repeats = 0;
};

Played play(mp::VideoGraph& graph, double from, double step = 0.001)
{
    Played out;
    double now = from;
    for (int guard = 0; guard < 200000; ++guard) {
        const mp::VideoGraph::Step step_taken = graph.pump(now);
        REQUIRE(step_taken != mp::VideoGraph::Step::failed);
        if (step_taken == mp::VideoGraph::Step::finished) {
            out.finished_at = now;
            return out;
        }
        if (step_taken == mp::VideoGraph::Step::repeated) {
            ++out.repeats;
            now += step;
        }
    }
    FAIL("the graph never finished");
    return out;
}

/// The picture that is up, as luminance extremes and whether it has colour in
/// it. The same three questions the decoder tests ask, and for the same reason:
/// they rule out a cleared buffer and a dropped chroma plane.
struct Picture {
    float darkest = 2.0f;
    float brightest = -1.0f;
    bool coloured = false;
};

Picture read_picture(mp::Presenter& presenter)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    MpPixelLayout layout{};
    REQUIRE(presenter.read_back(nullptr, 0, width, height, layout) == MP_ERR_NO_MEMORY);
    REQUIRE(width != 0);

    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4u);
    REQUIRE(presenter.read_back(pixels.data(), pixels.size() * sizeof(float), width, height,
                                layout) == MP_OK);

    Picture out;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const float r = pixels[i];
        const float g = pixels[i + 1];
        const float b = pixels[i + 2];
        const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        out.darkest = std::min(out.darkest, luminance);
        out.brightest = std::max(out.brightest, luminance);
        if (std::abs(r - b) > 0.05f || std::abs(g - b) > 0.05f) {
            out.coloured = true;
        }
    }
    return out;
}

} // namespace

TEST_CASE("a clock at the right speed shows every frame", "[video][graph][avsync]")
{
    Chain chain;
    OneStream feed{chain.demux};
    mp::VideoGraph graph{feed, chain.decoder, chain.presenter, chain.info};

    const Played run = play(graph, 0.0);
    const mp::VideoGraph::Stats stats = graph.stats();

    CHECK(stats.decoded == 24);
    CHECK(stats.shown == 24);
    CHECK(stats.dropped == 0);
    // Nothing was shown more than a millisecond late, which is the loop's own
    // step: the clock decided when, and the graph did not run ahead of it.
    CHECK(stats.worst_late_seconds > -0.0011);

    // And it took as long as the file is. A pump that presented on demand
    // rather than on the clock would have finished in microseconds.
    CHECK(run.finished_at > 0.9);
    CHECK(run.finished_at < 1.05);

    const Picture picture = read_picture(chain.presenter);
    CHECK(picture.darkest < 0.2f);
    CHECK(picture.brightest > 0.5f);
    CHECK(picture.coloured);
}

TEST_CASE("a clock that is ahead drops frames and never asks audio to wait",
          "[video][graph][avsync]")
{
    // Half a second of audio has played while nothing was decoded -- a stall, a
    // seek, a machine that cannot keep up with 4K. §8 says which side moves.
    Chain chain;
    OneStream feed{chain.demux};
    mp::VideoGraph graph{feed, chain.decoder, chain.presenter, chain.info};

    play(graph, 0.5);
    const mp::VideoGraph::Stats stats = graph.stats();

    CHECK(stats.decoded == 24);
    CHECK(stats.shown + stats.dropped == 24);
    // Half a second at 24000/1001 is twelve frames, give or take the one the
    // boundary falls on.
    CHECK(stats.dropped >= 10);
    CHECK(stats.dropped <= 13);
    CHECK(stats.shown > 0);

    // **And the picture is still right at the end.** Dropping frames is not
    // skipping decode: every frame was decoded, because a frame nobody decodes
    // is one the next one references.
    const Picture picture = read_picture(chain.presenter);
    CHECK(picture.darkest < 0.2f);
    CHECK(picture.brightest > 0.5f);
    CHECK(picture.coloured);
}

TEST_CASE("a clock that has stopped holds the picture", "[video][graph][avsync]")
{
    // The device is paused, or starved. Nothing is presented and nothing is
    // dropped: what is on screen stays, which is §8's duplicate and costs no
    // call at all.
    Chain chain;
    OneStream feed{chain.demux};
    mp::VideoGraph graph{feed, chain.decoder, chain.presenter, chain.info};

    // Far enough for one frame to be due, and no further.
    double now = 0.0;
    while (graph.pump(now) != mp::VideoGraph::Step::shown) {
        now += 0.001;
        REQUIRE(now < 1.0);
    }
    const std::uint64_t shown = graph.stats().shown;
    REQUIRE(shown == 1);

    for (int poll = 0; poll < 500; ++poll) {
        CHECK(graph.pump(now) == mp::VideoGraph::Step::repeated);
    }
    CHECK(graph.stats().shown == shown);
    CHECK(graph.stats().dropped == 0);
    // One frame was decoded and is being held. A graph that decoded ahead
    // would have a queue of frames whose pixels the decoder has already
    // promised to somebody else.
    CHECK(graph.stats().decoded == 2);
}

TEST_CASE("the graph reports what the decoder actually produced",
          "[video][graph][avsync]")
{
    // The container describes a stream and the bitstream describes itself, and
    // where they disagree the pixels came from the second one. dav1d states the
    // AV1 sequence header's colour, which for this fixture is what the MP4's
    // `colr` box says -- so the check is that nothing moved rather than that
    // something did, and the interesting half is that `timescale` survived.
    Chain chain;
    OneStream feed{chain.demux};
    const MpVideoInfo container = chain.info;
    mp::VideoGraph graph{feed, chain.decoder, chain.presenter, chain.info};

    double now = 0.0;
    while (graph.pump(now) != mp::VideoGraph::Step::shown) {
        now += 0.001;
        REQUIRE(now < 1.0);
    }

    const MpVideoInfo& after = graph.info();
    CHECK(after.width == container.width);
    CHECK(after.height == container.height);
    // **Never taken from the decoder**, which is what the codecs mean by
    // reporting zero: a decoder does not re-time a stream, so the container's
    // ticks-per-second and frame rate are the only ones there are.
    CHECK(after.timescale == container.timescale);
    CHECK(after.fps_num == container.fps_num);
    CHECK(after.fps_den == container.fps_den);
}

TEST_CASE("the video graph runs off one demuxer that audio is reading too",
          "[video][graph][router]")
{
    // **The shape a player actually has.** §4 says one file has one position,
    // so audio and video come out of one demuxer with both streams selected
    // and `PacketRouter` hands each side a feed. Nothing in `VideoGraph`
    // changes: it was written against `IPacketFeed` for this.
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    Module video_module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(demux_module.as<MpDemuxVtbl>() != nullptr);
    REQUIRE(codec_module.as<MpVideoCodecVtbl>() != nullptr);
    REQUIRE(video_module.as<MpVideoVtbl>() != nullptr);

    mp::Demux demux;
    REQUIRE(demux.open(*demux_module.as<MpDemuxVtbl>(), MEDIAPERCH_TEST_AV1) == MP_OK);

    std::uint32_t audio_stream = 0;
    std::uint32_t video_stream = 0;
    bool have_audio = false;
    bool have_video = false;
    MpStreamInfo stream_info{};
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        if (!demux.stream_info(i, stream_info)) {
            continue;
        }
        if (stream_info.kind == MP_STREAM_AUDIO && !have_audio) {
            audio_stream = i;
            have_audio = true;
        }
        if (stream_info.kind == MP_STREAM_VIDEO && !have_video) {
            video_stream = i;
            have_video = true;
        }
    }
    REQUIRE(have_audio);
    REQUIRE(have_video);

    MpVideoInfo info{};
    info.size = sizeof(info);
    REQUIRE(demux.video_info(video_stream, info));

    mp::Presenter presenter;
    REQUIRE(presenter.open(*video_module.as<MpVideoVtbl>(), nullptr) == MP_OK);
    REQUIRE(presenter.set("device", "warp") == MP_OK);
    REQUIRE(presenter.configure(info) == MP_OK);
    MpGraphicsDevice device{};
    REQUIRE(presenter.get_device(device) == MP_OK);

    std::vector<std::uint8_t> config;
    (void)demux.stream_config(video_stream, config);
    mp::VideoDecoder decoder;
    REQUIRE(decoder.open(*codec_module.as<MpVideoCodecVtbl>(), MP_CODEC_AV1, &device,
                         config.data(),
                         static_cast<std::uint32_t>(config.size())) == MP_OK);

    const std::uint32_t both[] = {audio_stream, video_stream};
    REQUIRE(demux.select_streams(both) == MP_OK);
    mp::PacketRouter router{demux, both};
    mp::IPacketFeed* video_feed = router.feed(video_stream);
    mp::IPacketFeed* audio_feed = router.feed(audio_stream);
    REQUIRE(video_feed != nullptr);
    REQUIRE(audio_feed != nullptr);

    mp::VideoGraph graph{*video_feed, decoder, presenter, info};

    // The audio side is a player's decode thread, standing in for itself: it
    // takes what is its own and keeps taking, which is what stops the video
    // side's reads from queueing the whole track.
    std::vector<std::uint8_t> audio_buffer;
    MpPacket audio_packet{};
    std::uint64_t audio_packets = 0;
    bool audio_done = false;
    const auto drain_audio = [&] {
        while (!audio_done) {
            const MpResult r = audio_feed->next(audio_buffer, audio_packet);
            if (r == MP_OK) {
                ++audio_packets;
                continue;
            }
            if (r == MP_END) {
                audio_done = true;
            }
            return; // MP_ERR_BUSY means the video side has to read first
        }
    };

    double now = 0.0;
    for (int guard = 0; guard < 200000; ++guard) {
        drain_audio();
        const mp::VideoGraph::Step step = graph.pump(now);
        REQUIRE(step != mp::VideoGraph::Step::failed);
        if (step == mp::VideoGraph::Step::finished) {
            break;
        }
        if (step == mp::VideoGraph::Step::repeated) {
            now += 0.001;
        }
    }
    REQUIRE(graph.finished());

    CHECK(graph.stats().shown == 24);
    CHECK(graph.stats().dropped == 0);
    CHECK(audio_packets > 0);
    // Both sides read the whole file once, and nothing is still waiting.
    CHECK(router.stats().queued_packets == 0);
    // And the interleave is what stops the queues being the whole track: the
    // peak is a fraction of the file rather than one side of it.
    CHECK(router.stats().peak_queued_bytes > 0);
}

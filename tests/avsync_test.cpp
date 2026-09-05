// SPDX-License-Identifier: GPL-3.0-or-later
//
// §8, with no sound card and no display.
//
// **The clock is a number a test chooses**, which is the whole reason `AvClock`
// takes a reading rather than reading one. Every question §8 asks is a question
// about a device that is running fast, or slow, or has stopped, and none of
// those can be arranged on real hardware -- so they are arranged here, exactly,
// and the answers are checked against arithmetic rather than against a
// stopwatch.
//
// The other half is the rule itself: **video drops or duplicates against the
// audio clock, always, and audio is never moved.** That last part is not a
// test, it is the absence of one -- there is no method anywhere in this file's
// subject that moves audio, and `mp::AvClock` has no way to ask a sink for
// anything at all.

#include "mediaperch/avsync.hpp"

#include "fake_sink.hpp"
#include "mediaperch/packet.hpp"
#include "module_loader.hpp"
#include "mediaperch/format.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/source.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using Catch::Approx;
using mp::test::Module;

namespace {

/// A device that counts at 10 MHz, which is what QueryPerformanceCounter does
/// on every machine this has been run on. The number does not matter to the
/// arithmetic and is written out so the tests below can be read as times.
constexpr std::uint64_t k_tick_rate = 10'000'000;

/// One second of ticks, so a test can say "a second later" and mean it.
constexpr std::uint64_t k_second = k_tick_rate;

mp::ClockSpec plain_spec(std::uint32_t rate = 48000)
{
    mp::ClockSpec spec;
    spec.wire_rate = rate;
    spec.source_rate = rate;
    spec.tick_rate = k_tick_rate;
    return spec;
}

} // namespace

TEST_CASE("a clock nobody has read yet answers nothing", "[avsync][clock]")
{
    mp::AvClock clock;
    clock.configure(plain_spec());
    // **Not a plausible zero.** Before a reading there is no master clock, and
    // a host that synchronised video to a guess would be inventing the one
    // thing §8 says everything else follows.
    CHECK_FALSE(clock.ready());

    clock.observe({.device_frames = 0, .ticks = 1000});
    CHECK(clock.ready());
}

TEST_CASE("the clock keeps running between readings", "[avsync][clock]")
{
    // A reading is out of date the moment it is taken -- the device does not
    // stop while a caller thinks about it -- so the tick it was taken at is
    // half of what `get_position` returns, and this is what that half is for.
    mp::AvClock clock;
    clock.configure(plain_spec());
    clock.observe({.device_frames = 48000, .ticks = 100 * k_second});

    CHECK(clock.audible_frames(100 * k_second) == Approx(48000.0));
    CHECK(clock.audible_frames(100 * k_second + k_second / 2) == Approx(72000.0));
    CHECK(clock.audible_seconds(101 * k_second) == Approx(2.0));

    // Asked about a moment before the reading, which happens whenever a caller
    // holds a timestamp from earlier in the same iteration. The subtraction
    // runs backwards rather than wrapping around zero.
    CHECK(clock.audible_frames(99 * k_second) == Approx(0.0));
    CHECK(clock.audible_frames(99 * k_second + k_second / 2) == Approx(24000.0));
}

TEST_CASE("what is audible is behind what the device has played", "[avsync][clock]")
{
    // §5's number, arriving where it was always meant to: a linear-phase stage
    // delays the audio by half its filter, so the sound leaving the endpoint
    // came from source material that far back.
    mp::ClockSpec spec = plain_spec();
    spec.latency_frames = 4800; // 100 ms of chain
    mp::AvClock clock;
    clock.configure(spec);
    clock.observe({.device_frames = 48000, .ticks = 0});

    CHECK(clock.audible_seconds(0) == Approx(0.9));

    // And the same device position with no chain in front of it is a tenth of
    // a second further on. A video frame timed against the wrong one of these
    // is a hundred milliseconds out, which is well past visible.
    mp::AvClock bare;
    bare.configure(plain_spec());
    bare.observe({.device_frames = 48000, .ticks = 0});
    CHECK(bare.audible_seconds(0) == Approx(1.0));
}

TEST_CASE("the anchor is what a seek moves", "[avsync][clock]")
{
    // A seek does not stop the device: the audio already committed still
    // plays, so the frame seeked to becomes audible when the device reaches
    // what had been *written* at that moment. Both numbers are ones the graphs
    // have kept since gapless was written.
    mp::ClockSpec spec = plain_spec();
    spec.origin_device_frame = 96000; // two seconds had been written
    spec.origin_source_frame = 480000; // and the seek landed ten seconds in
    mp::AvClock clock;
    clock.configure(spec);

    // The device has not reached the written point yet: what is playing is
    // still the audio from before the seek, and the position it belongs to is
    // not one anybody is at. Clamped to the anchor rather than run backwards.
    clock.observe({.device_frames = 48000, .ticks = 0});
    CHECK(clock.audible_seconds(0) == Approx(10.0));

    // And past it, the new position counts from the seek.
    clock.observe({.device_frames = 96000 + 24000, .ticks = 0});
    CHECK(clock.audible_seconds(0) == Approx(10.5));
}

TEST_CASE("the device counts in its own frames and the file in the file's",
          "[avsync][clock]")
{
    // A resampler in the chain makes these different numbers, and
    // `position_frames` has applied the ratio since it was written rather than
    // assuming it away. The clock applies the same one.
    mp::ClockSpec spec;
    spec.wire_rate = 96000;
    spec.source_rate = 48000;
    spec.tick_rate = k_tick_rate;
    mp::AvClock clock;
    clock.configure(spec);
    clock.observe({.device_frames = 96000, .ticks = 0});

    CHECK(clock.audible_frames(0) == Approx(48000.0));
    // A second of wire frames is a second of source seconds either way, which
    // is the point: seconds are what video is compared against.
    CHECK(clock.audible_seconds(0) == Approx(1.0));
}

TEST_CASE("nothing accumulates, so nothing drifts", "[avsync][clock]")
{
    // The argument the header makes, made against a number. An hour of a
    // clock read once a second is 3600 answers, and the last one is exact --
    // because it is computed from the last reading and not from the ones
    // before it. A player that integrated would be somewhere else by now.
    mp::AvClock clock;
    clock.configure(plain_spec(44100));

    double last = 0.0;
    for (std::uint64_t second = 0; second < 3600; ++second) {
        clock.observe({.device_frames = 44100 * second, .ticks = second * k_second});
        last = clock.audible_seconds(second * k_second);
    }
    CHECK(last == Approx(3599.0));

    // And a reading that is wrong is wrong once. The next one replaces it
    // whole rather than correcting a total.
    clock.observe({.device_frames = 0, .ticks = 3599 * k_second});
    CHECK(clock.audible_seconds(3599 * k_second) == Approx(0.0));
    clock.observe({.device_frames = 44100 * 3599, .ticks = 3599 * k_second});
    CHECK(clock.audible_seconds(3599 * k_second) == Approx(3599.0));
}

TEST_CASE("a frame is shown when its time comes, and not before", "[avsync][pacer]")
{
    // 24000/1001, which is the rate this tree's fixtures are, and the reason
    // MpVideoInfo states a ratio: 41.708333... ms is not a decimal.
    mp::VideoPacer pacer;
    pacer.configure(24000, 24000, 1001);
    const double interval = pacer.interval_seconds();
    CHECK(interval == Approx(1001.0 / 24000.0));

    // Frame 10 of a 24000-tick timescale is at 10 * 1001 ticks.
    constexpr std::uint64_t pts = 10 * 1001;
    const double due = 10.0 * 1001.0 / 24000.0;

    SECTION("early is a repeat, and asking again changes nothing")
    {
        const auto first = pacer.decide(pts, due - 0.02);
        CHECK(first.fate == mp::FrameFate::repeat);
        CHECK(first.error_seconds == Approx(0.02));
        // The same frame, asked about again a moment later. A caller polling a
        // frame that is not due must be able to, which means no state moved.
        const auto again = pacer.decide(pts, due - 0.01);
        CHECK(again.fate == mp::FrameFate::repeat);
        CHECK(pacer.stats().shown == 0);
        CHECK(pacer.stats().dropped == 0);
    }

    SECTION("on time is shown")
    {
        CHECK(pacer.decide(pts, due).fate == mp::FrameFate::show);
        CHECK(pacer.stats().shown == 1);
    }

    SECTION("a little late is still shown")
    {
        const auto late = pacer.decide(pts, due + interval * 0.9);
        CHECK(late.fate == mp::FrameFate::show);
        CHECK(late.error_seconds == Approx(-interval * 0.9));
    }

    SECTION("more than a frame late is dropped")
    {
        // Past one interval the *next* frame is already due, so showing this
        // one only makes that one later too.
        CHECK(pacer.decide(pts, due + interval * 1.1).fate == mp::FrameFate::drop);
        CHECK(pacer.stats().dropped == 1);
        CHECK(pacer.stats().shown == 0);
    }
}

TEST_CASE("with a lead, a frame is put on the nearest refresh", "[avsync][pacer]")
{
    // **The measured improvement, pinned.** A decision made at a vertical blank
    // shows a frame at the *next* one, so asking "is it due now" answers the
    // wrong question and costs up to a whole refresh -- measured at 16.6 ms
    // against a 16.7 ms refresh, which is the floor of that question rather
    // than of the clock. With the lead given, the frame chosen is the one whose
    // time the presentation instant is nearest to, and the error straddles zero
    // instead of sitting on one side of it: 8.4 ms late to 8.1 ms early on the
    // same machine, against a 16.3 ms refresh.
    constexpr double refresh = 1.0 / 60.0;
    mp::VideoPacer pacer;
    pacer.configure(1000, 25, 1); // milliseconds, 40 ms a frame
    pacer.set_lead_seconds(refresh);

    // A frame due at one second. The clock is read at the decision; the frame
    // appears `refresh` later.
    constexpr std::uint64_t pts = 1000;

    // Still more than half a refresh away from the next presentation: waiting
    // puts it nearer than showing it now would.
    CHECK(pacer.decide(pts, 1.0 - refresh - refresh).fate == mp::FrameFate::repeat);

    // Within half a refresh of this presentation, so this one is the nearest.
    // **Shown early**, which the old rule could never do and which is the whole
    // of the improvement: half a refresh early beats half a refresh late,
    // because the alternative was a whole one.
    const auto early = pacer.decide(pts, 1.0 - refresh - refresh * 0.4);
    CHECK(early.fate == mp::FrameFate::show);
    CHECK(early.error_seconds == Approx(refresh * 0.4));
}

TEST_CASE("without a lead nothing is shown before its time", "[avsync][pacer]")
{
    // The default, and what a caller with no display wants: the arithmetic
    // tests above, and anything measuring rather than presenting.
    mp::VideoPacer pacer;
    pacer.configure(1000, 25, 1);
    CHECK(pacer.decide(1000, 0.999).fate == mp::FrameFate::repeat);
    CHECK(pacer.decide(1000, 1.0).fate == mp::FrameFate::show);
}

TEST_CASE("a container that states no rate has one measured from it",
          "[avsync][pacer]")
{
    // Matroska states a duration per frame rather than a rate, and a container
    // that timestamps every frame may state neither -- MpVideoInfo says 0/0 is
    // normal. The interval is then the distance between the timestamps
    // themselves.
    mp::VideoPacer pacer;
    pacer.configure(1000, 0, 0); // milliseconds, no stated rate
    CHECK(pacer.interval_seconds() == Approx(0.0));

    // **Nothing is dropped before the interval is known.** One timestamp is
    // not an interval, and a frame dropped on a guessed rate is gone.
    CHECK(pacer.decide(0, 100.0).fate == mp::FrameFate::show);
    CHECK(pacer.stats().dropped == 0);

    CHECK(pacer.decide(40, 0.040).fate == mp::FrameFate::show);
    CHECK(pacer.interval_seconds() == Approx(0.040));

    // And now it can be late enough to drop.
    CHECK(pacer.decide(80, 0.080 + 0.050).fate == mp::FrameFate::drop);
}

TEST_CASE("the two timelines are put on the same origin", "[avsync][pacer]")
{
    // §9.9 measured sixty milliseconds of this in the tree's own fixture: the
    // engine applies the audio track's edit before the source counts a frame,
    // and video timestamps stay container-relative. Somebody has to subtract
    // it, once.
    mp::VideoPacer pacer;
    pacer.configure(1000, 25, 1);
    pacer.set_skew_seconds(-0.060);

    // A frame stamped at one second is due at 0.940 on the audio's timeline.
    CHECK(pacer.decide(1000, 0.930).fate == mp::FrameFate::repeat);
    CHECK(pacer.decide(1000, 0.940).fate == mp::FrameFate::show);
}

TEST_CASE("a stream with no timescale is played rather than timed",
          "[avsync][pacer]")
{
    // Zero means "not stated" and never "zero" -- §9.9's rule for every field
    // in MpStreamInfo. Both demuxers here always state it, so this is a guard
    // rather than a path, and the only thing left that is not a guess is to
    // show each frame as it decodes.
    mp::VideoPacer pacer;
    pacer.configure(0, 24000, 1001);
    CHECK(pacer.decide(999999, 0.0).fate == mp::FrameFate::show);
    CHECK(pacer.stats().dropped == 0);
}

TEST_CASE("a seek forgets the interval it had measured", "[avsync][pacer]")
{
    mp::VideoPacer pacer;
    pacer.configure(1000, 0, 0);
    CHECK(pacer.decide(0, 0.0).fate == mp::FrameFate::show);
    CHECK(pacer.decide(40, 0.040).fate == mp::FrameFate::show);
    CHECK(pacer.interval_seconds() == Approx(0.040));

    // The timestamp after a seek is not the one after the last, so the
    // difference between them is not an interval.
    pacer.reset();
    CHECK(pacer.interval_seconds() == Approx(0.0));
    CHECK(pacer.decide(600000, 600.0 + 10.0).fate == mp::FrameFate::show);
}

// --------------------------------------------------------------------------
// The graph, which is the thing that knows the numbers
// --------------------------------------------------------------------------

namespace {

/// A source of silence, long enough that a run does not end mid-test.
class Silence final : public mp::ISource {
public:
    explicit Silence(mp::Format format) : format_(format) {}
    [[nodiscard]] const mp::Format& format() const noexcept override { return format_; }
    std::size_t read(void* dst, std::size_t bytes) override
    {
        std::memset(dst, 0, bytes);
        return bytes;
    }

private:
    mp::Format format_;
};

mp::Format stereo_s16(std::uint32_t rate)
{
    mp::Format f{};
    f.sample_rate = rate;
    f.channels = 2;
    f.sample_type = mp::SampleType::s16;
    f.encoding = mp::Encoding::pcm;
    return f;
}

} // namespace

TEST_CASE("a sink with no clock is a graph that says so", "[avsync][graph]")
{
    // A module that did not implement `get_position` leaves the vtable entry
    // null. There is then no master clock, and the honest answer is that there
    // is none rather than a number that looks like one.
    mp::test::FakeSink device{mp::test::FakeSinkRules{}};
    mp::Sink sink = device.handle();
    Silence source{stereo_s16(48000)};
    mp::PassthroughGraph graph{source, sink, stereo_s16(48000), 64, mp::Fidelity::exact};

    mp::ClockReading reading{};
    CHECK_FALSE(graph.read_clock(reading));
}

TEST_CASE("the graph states the clock's fixed facts", "[avsync][graph]")
{
    mp::test::FakeSinkRules rules;
    rules.has_clock = true;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();
    Silence source{stereo_s16(44100)};
    mp::PassthroughGraph graph{source, sink, stereo_s16(44100), 64, mp::Fidelity::exact};

    // A run that begins part way in -- `--seek`, or a resume after the device
    // was pulled out -- and the anchor says so.
    graph.set_position(44100 * 30);

    const mp::ClockSpec spec = graph.clock_spec();
    CHECK(spec.wire_rate == 44100);
    CHECK(spec.source_rate == 44100);
    CHECK(spec.origin_source_frame == 44100 * 30);
    CHECK(spec.origin_device_frame == 0);
    // Path A has no chain in it, which is what makes it Path A.
    CHECK(spec.latency_frames == 0);

    device.set_clock(44100, 7 * k_second);
    mp::ClockReading reading{};
    REQUIRE(graph.read_clock(reading));
    CHECK(reading.device_frames == 44100);
    CHECK(reading.ticks == 7 * k_second);

    // And the two together are a time: a second of device frames past a run
    // that began thirty seconds in.
    mp::AvClock clock;
    mp::ClockSpec with_ticks = spec;
    with_ticks.tick_rate = k_tick_rate;
    clock.configure(with_ticks);
    clock.observe(reading);
    CHECK(clock.audible_seconds(7 * k_second) == Approx(31.0));
}

TEST_CASE("what is audible is behind what the device was given", "[avsync][graph]")
{
    // The gap `position_frames` documents, as a number. The graph writes a
    // device buffer ahead of what is playing, which is the right answer for
    // resuming and the wrong one for timing a picture against.
    mp::test::FakeSinkRules rules;
    rules.has_clock = true;
    rules.period_frames = 480; // 10 ms at 48 kHz
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();
    Silence source{stereo_s16(48000)};
    mp::PassthroughGraph graph{source, sink, stereo_s16(48000), 480, mp::Fidelity::exact};

    mp::ClockSpec spec = graph.clock_spec();
    spec.tick_rate = k_tick_rate;
    mp::AvClock clock;
    clock.configure(spec);

    // The device has played a second; three buffers beyond that had been
    // handed over and are not audible yet.
    device.set_clock(48000, 0);
    mp::ClockReading reading{};
    REQUIRE(graph.read_clock(reading));
    clock.observe(reading);

    CHECK(clock.audible_seconds(0) == Approx(1.0));
    // Which is 30 ms behind where a caller reading the written count would
    // have put it -- and a video frame timed against that would be a frame and
    // a half early on a 24 fps stream.
    const double written = 1.0 + 3.0 * 480.0 / 48000.0;
    CHECK(written - clock.audible_seconds(0) == Approx(0.030));
}

// --------------------------------------------------------------------------
// A real stream, against a clock that is not real
// --------------------------------------------------------------------------

#if defined(MEDIAPERCH_CODEC_DAV1D) && defined(MEDIAPERCH_TEST_AV1) && \
    defined(MEDIAPERCH_DEMUX_MP4)

namespace {

/// The presentation timestamps of a real file, in the order a presenter gets
/// them.
///
/// **Decoded rather than read off the packets**, and the difference is the
/// point. An MP4 stores samples in decode order and this fixture has B-frames
/// in it -- §9.9 is where that was found -- so the packet timestamps do not
/// come out in the order the frames are shown. A decoder reorders, and what it
/// hands over is what the pacer is for. Reading the packets and sorting them
/// would have made the same list and proved nothing about who does the sorting.
std::vector<std::uint64_t> decoded_pts(MpVideoInfo& info)
{
    Module demux_module{MEDIAPERCH_DEMUX_MP4, MP_KIND_DEMUX};
    Module codec_module{MEDIAPERCH_CODEC_DAV1D, MP_KIND_VCODEC};
    REQUIRE(demux_module.as<MpDemuxVtbl>() != nullptr);
    REQUIRE(codec_module.as<MpVideoCodecVtbl>() != nullptr);
    const auto* codec = codec_module.as<MpVideoCodecVtbl>();

    mp::Demux demux;
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

    info = MpVideoInfo{};
    info.size = sizeof(info);
    REQUIRE(demux.video_info(stream, info));

    std::vector<std::uint8_t> config;
    (void)demux.stream_config(stream, config);
    MpVideoCodec* decoder = nullptr;
    REQUIRE(codec->open(MP_CODEC_AV1, nullptr, config.data(),
                        static_cast<std::uint32_t>(config.size()), &decoder) == MP_OK);

    const std::uint32_t only_video[] = {stream};
    REQUIRE(demux.select_streams(only_video) == MP_OK);

    std::vector<std::uint64_t> out;
    std::vector<std::uint8_t> buffer;
    MpPacket packet{};
    const auto drain = [&] {
        for (int guard = 0; guard < 64; ++guard) {
            MpVideoFrame frame{};
            frame.size = sizeof(frame);
            if (codec->next_frame(decoder, &frame) != MP_OK) {
                return;
            }
            out.push_back(frame.pts);
        }
    };
    while (demux.read_packet(buffer, packet) == MP_OK) {
        REQUIRE(codec->decode(decoder, buffer.data(), packet.bytes, packet.frame) == MP_OK);
        drain();
    }
    REQUIRE(codec->flush(decoder) == MP_OK);
    drain();
    codec->close(decoder);
    return out;
}

/// A presenter's loop with time made of arithmetic: it holds one frame, asks,
/// and comes back a millisecond later when the answer is `repeat`.
struct Run {
    std::uint64_t shown = 0;
    std::uint64_t dropped = 0;
    std::uint64_t repeats = 0;
    double finished_at = 0.0;
};

Run pace(mp::VideoPacer& pacer, const std::vector<std::uint64_t>& pts, double from,
         double step)
{
    Run run;
    double now = from;
    for (std::size_t index = 0; index < pts.size();) {
        const auto decision = pacer.decide(pts[index], now);
        if (decision.fate == mp::FrameFate::repeat) {
            ++run.repeats;
            now += step;
            REQUIRE(run.repeats < 100000); // a loop that never advances is a bug
            continue;
        }
        if (decision.fate == mp::FrameFate::show) {
            ++run.shown;
        } else {
            ++run.dropped;
        }
        ++index;
    }
    run.finished_at = now;
    return run;
}

} // namespace

TEST_CASE("a real stream, kept by the audio clock", "[avsync][stream]")
{
    MpVideoInfo info{};
    const std::vector<std::uint64_t> pts = decoded_pts(info);
    REQUIRE(pts.size() == 24);
    REQUIRE(info.timescale != 0);

    // Presentation order, which is what a decoder hands over and what the
    // packets were not in.
    for (std::size_t i = 1; i < pts.size(); ++i) {
        CHECK(pts[i] > pts[i - 1]);
    }

    const double first = mp::stream_seconds(pts.front(), info.timescale);
    const double last = mp::stream_seconds(pts.back(), info.timescale);

    SECTION("a clock at the right speed shows every frame")
    {
        mp::VideoPacer pacer;
        pacer.configure(info.timescale, info.fps_num, info.fps_den);
        const Run run = pace(pacer, pts, first, 0.001);
        CHECK(run.shown == 24);
        CHECK(run.dropped == 0);
        // And it took as long as the file is: the clock decided when, and
        // nothing here ran ahead of it.
        CHECK(run.finished_at == Approx(last).margin(0.002));
    }

    SECTION("a clock that is already ahead drops until the picture catches up")
    {
        // Half a second of audio played while the video was not being decoded
        // -- a seek, a stall, a machine that could not keep up. §8 says which
        // one moves: the frames that are past go, and the audio is untouched.
        mp::VideoPacer pacer;
        pacer.configure(info.timescale, info.fps_num, info.fps_den);
        const Run run = pace(pacer, pts, first + 0.5, 0.001);
        CHECK(run.dropped > 0);
        CHECK(run.shown > 0);
        CHECK(run.shown + run.dropped == 24);
        // Twelve frames is half a second at this rate, give or take the one
        // the boundary falls on.
        CHECK(run.dropped >= 11);
        CHECK(run.dropped <= 13);
    }

    SECTION("a clock that has stopped holds the picture")
    {
        // The device is paused, or starved. Nothing is shown and nothing is
        // dropped: the frame already on screen stays up, which is the
        // duplicate §8 describes, and it costs no decision at all.
        mp::VideoPacer pacer;
        pacer.configure(info.timescale, info.fps_num, info.fps_den);
        std::uint64_t repeats = 0;
        for (int poll = 0; poll < 1000; ++poll) {
            // The second frame, with the clock stuck at the first's time.
            const auto decision = pacer.decide(pts[1], first);
            CHECK(decision.fate == mp::FrameFate::repeat);
            ++repeats;
        }
        CHECK(repeats == 1000);
        CHECK(pacer.stats().shown == 0);
        CHECK(pacer.stats().dropped == 0);
    }
}

#endif

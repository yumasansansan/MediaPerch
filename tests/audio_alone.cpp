// SPDX-License-Identifier: GPL-3.0-or-later
//
// **The audio engine, and nothing else.** This program links MediaPerch::audio
// and MediaPerch::core and no video library; if the audio engine ever came to
// need a symbol of the video one, this is where the link would fail, and
// nowhere else. It plays a tone into a fake device for a moment, reads the
// device back as a clock, and checks that frames came out.

#include "fake_sink.hpp"

#include "mediaperch/clock.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/sine.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    mp::test::FakeSinkRules rules;
    rules.period_frames = 64;
    rules.pace_us = 500;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const mp::Format format{.sample_rate = 44100,
                            .channels = 2,
                            .channel_mask = 0,
                            .sample_type = mp::SampleType::s16,
                            .encoding = mp::Encoding::pcm,
                            .valid_bits = 0};
    mp::SineSource tone{format, 1000.0, 0.5};
    const mp::Negotiated negotiated = mp::negotiate_best(sink, format, mp::PathPolicy::bit_exact);
    if (!negotiated.ok) {
        std::fprintf(stderr, "audio alone: the fake device took none of the formats\n");
        return 1;
    }

    mp::PassthroughGraph graph{tone, sink, negotiated.accepted, 64, negotiated.fidelity};
    if (graph.start() != MP_OK) {
        std::fprintf(stderr, "audio alone: the graph would not start\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    // The device, as the clock the other engine would follow -- answered here
    // without that engine being anywhere in the process.
    mp::GraphClock<mp::PassthroughGraph> clock{graph};
    mp::ClockReading reading{};
    const bool ticking = clock.read(reading);
    graph.stop();

    const auto stats = graph.stats();
    std::printf("audio alone: %llu frames rendered, %llu underruns, clock %s at %llu\n",
                static_cast<unsigned long long>(stats.frames_rendered),
                static_cast<unsigned long long>(stats.underruns), ticking ? "read" : "silent",
                static_cast<unsigned long long>(reading.device_frames));
    // The fake device keeps no position of its own, so the reading says so;
    // what the clock must answer regardless is the spec, which is the graph's.
    return stats.frames_rendered > 0 && clock.spec().wire_rate == 44100 ? 0 : 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_sink.hpp"

#include "mediaperch/passthrough.hpp"
#include "mediaperch/processed.hpp"
#include "mediaperch/repack.hpp"
#include "mediaperch/sink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
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

/// A source with an end, holding a known pattern, so the bytes that reach the
/// device can be compared with the bytes that left the decoder.
class VectorSource final : public mp::ISource {
public:
    VectorSource(const mp::Format& format, std::vector<std::uint8_t> data)
        : format_(format), data_(std::move(data))
    {
    }

    [[nodiscard]] const mp::Format& format() const noexcept override { return format_; }

    std::size_t read(void* dst, std::size_t bytes) override
    {
        const std::size_t stride = mp::frame_bytes(format_);
        const std::size_t left = data_.size() - offset_;
        const std::size_t n = std::min(bytes - (bytes % stride), left);
        std::memcpy(dst, data_.data() + offset_, n);
        offset_ += n;
        return n;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }

private:
    mp::Format format_;
    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
};

std::vector<std::uint8_t> pattern(std::size_t bytes)
{
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        // Not a ramp: a ramp would survive an off-by-one shift unnoticed.
        out[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    return out;
}

/// Templated because Path A and Path B are two classes: from out here the only
/// thing that matters is that both of them stop.
template <typename Graph>
bool wait_until_stopped(const Graph& graph)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (graph.running()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

} // namespace

// --------------------------------------------------------------------------
// negotiate_best
// --------------------------------------------------------------------------

TEST_CASE("negotiation stops at the first format the device really takes", "[negotiate]")
{
    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format& f) { return f.sample_type == mp::SampleType::s16; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto result = mp::negotiate_best(sink, cd_audio());

    CHECK(result.ok);
    CHECK(result.fidelity == mp::Fidelity::exact);
    CHECK(result.tried == 1);
    CHECK(result.accepted == cd_audio());
}

TEST_CASE("a device that only takes the extensible form does not cause a widening",
          "[negotiate]")
{
    // The reason the mask variant is paired with its base rather than appended
    // after every widening: this device wants a channel mask and nothing else,
    // and must not end up with a wider container it never asked for.
    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format& f) { return f.channel_mask != 0; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto result = mp::negotiate_best(sink, cd_audio());

    REQUIRE(result.ok);
    CHECK(result.fidelity == mp::Fidelity::exact);
    CHECK(result.channel_mask_added);
    CHECK(result.accepted.sample_type == mp::SampleType::s16);
    CHECK(result.tried == 2);
}

TEST_CASE("a 24-bit-only device gets a repacked stream, not a converted one", "[negotiate]")
{
    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format& f) {
        return f.sample_type == mp::SampleType::s24_in_32;
    };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto result = mp::negotiate_best(sink, cd_audio());

    REQUIRE(result.ok);
    CHECK(result.fidelity == mp::Fidelity::repacked);
    CHECK(mp::is_bit_exact(result.fidelity));
    CHECK(mp::effective_valid_bits(result.accepted) == 16);
}

TEST_CASE("a device that says yes and means something else has failed", "[negotiate]")
{
    // The failure mode this whole design exists to refuse: MP_OK plus a format
    // that is not what was asked for. Taking that on trust is how a player ends
    // up silently resampling and still calling itself bit-exact.
    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format&) { return true; };
    rules.distort = [](const mp::Format& f) {
        mp::Format changed = f;
        changed.sample_rate = 48000;
        return changed;
    };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto result = mp::negotiate_best(sink, cd_audio());

    CHECK_FALSE(result.ok);
    CHECK(result.last_error == MP_ERR_FORMAT);
    CHECK(result.tried > 1); // every candidate was offered and every one lied
}

TEST_CASE("a device that refuses everything reports what it refused", "[negotiate]")
{
    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format&) { return false; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto result = mp::negotiate_best(sink, cd_audio());

    CHECK_FALSE(result.ok);
    CHECK(result.last_error == MP_ERR_FORMAT);
    CHECK(result.tried == device.offered().size());
    CHECK(result.tried == mp::build_candidates(cd_audio()).size());
}

// --------------------------------------------------------------------------
// PassthroughGraph
// --------------------------------------------------------------------------

TEST_CASE("every byte the source produced reaches the device unchanged", "[passthrough]")
{
    // This is the test the project is for. The source is sized to fit entirely
    // in the ring during the prefill, so the comparison measures the data path
    // and not the scheduler.
    constexpr std::uint32_t period = 64;
    const auto format = cd_audio();
    const std::size_t stride = mp::frame_bytes(format);
    const auto bytes = pattern(stride * period * 4);

    VectorSource source{format, bytes};
    mp::test::FakeSinkRules rules;
    rules.period_frames = period;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    mp::Format accepted{};
    REQUIRE(sink.negotiate(format, accepted) == MP_OK);
    REQUIRE(accepted == format);

    mp::PassthroughGraph graph{source, sink, accepted, period, mp::Fidelity::exact};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();

    const auto captured = device.captured();
    REQUIRE(captured.size() >= bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), captured.begin()));
    CHECK(graph.error() == MP_OK);
    CHECK(graph.stats().underruns == 0);
}

TEST_CASE("a repacked stream reaches the device as the move and nothing else",
          "[passthrough]")
{
    constexpr std::uint32_t period = 64;
    const auto source_format = cd_audio();

    mp::Format wire = source_format;
    wire.sample_type = mp::SampleType::s24_in_32;
    wire.valid_bits = 16;

    const std::size_t source_stride = mp::frame_bytes(source_format);
    const auto bytes = pattern(source_stride * period * 4);
    VectorSource source{source_format, bytes};

    mp::test::FakeSinkRules rules;
    rules.period_frames = period;
    rules.accepts = [](const mp::Format& f) {
        return f.sample_type == mp::SampleType::s24_in_32;
    };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto negotiated = mp::negotiate_best(sink, source_format);
    REQUIRE(negotiated.ok);
    REQUIRE(negotiated.fidelity == mp::Fidelity::repacked);

    mp::PassthroughGraph graph{source, sink, negotiated.accepted, period,
                               negotiated.fidelity};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();

    // Compute the expectation independently rather than reusing the graph's path.
    const std::size_t samples = bytes.size() / 2;
    std::vector<std::uint8_t> expected(samples * 4);
    REQUIRE(mp::repack(bytes.data(), mp::SampleType::s16, expected.data(),
                       mp::SampleType::s24_in_32, 16, samples));

    const auto captured = device.captured();
    REQUIRE(captured.size() >= expected.size());
    CHECK(std::equal(expected.begin(), expected.end(), captured.begin()));
    CHECK(graph.error() == MP_OK);
}

TEST_CASE("a graph refuses to run a conversion", "[passthrough]")
{
    // Path B is a different graph, and asking this one to do it is a bug in the
    // caller, not something to paper over.
    const auto format = cd_audio();
    VectorSource source{format, pattern(1024)};
    mp::test::FakeSink device{mp::test::FakeSinkRules{}};
    mp::Sink sink = device.handle();

    mp::Format accepted{};
    REQUIRE(sink.negotiate(format, accepted) == MP_OK);

    mp::PassthroughGraph graph{source, sink, accepted, 64, mp::Fidelity::converted};
    CHECK(graph.start() == MP_ERR_INVALID);
}

TEST_CASE("a device that stops signalling is reported, not waited on for ever",
          "[passthrough]")
{
    const auto format = cd_audio();
    VectorSource source{format, pattern(4096)};

    mp::test::FakeSinkRules rules;
    rules.period_frames = 64;
    rules.timeouts_before_ok = 1'000'000; // never signals again
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    mp::Format accepted{};
    REQUIRE(sink.negotiate(format, accepted) == MP_OK);

    mp::PassthroughGraph graph{source, sink, accepted, 64, mp::Fidelity::exact};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));

    CHECK(graph.error() == MP_TIMEOUT);
    CHECK(graph.stats().wait_timeouts == 1);
}

TEST_CASE("a device is only ever asked for what the policy allows", "[negotiate][path]")
{
    // A device that will not take the source's own container but will take a
    // repack. Under `automatic` that plays; under `exact_only` it must not,
    // because a repack is not the memcpy that was asked for.
    mp::Format source = cd_audio();
    source.sample_type = mp::SampleType::s24_in_32;

    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format& f) {
        return f.sample_type == mp::SampleType::s24_packed;
    };
    mp::test::FakeSink accepts_packed{rules};
    mp::Sink sink = accepts_packed.handle();
    const auto automatic = mp::negotiate_best(sink, source, mp::PathPolicy::automatic);
    REQUIRE(automatic.ok);
    CHECK(automatic.fidelity == mp::Fidelity::repacked);

    mp::test::FakeSink again{rules};
    mp::Sink strict_sink = again.handle();
    const auto strict = mp::negotiate_best(strict_sink, source, mp::PathPolicy::exact_only);
    CHECK_FALSE(strict.ok);
    CHECK(strict.policy == mp::PathPolicy::exact_only);
    // And it was never even offered: two candidates, both the source's own
    // container. Negotiating costs an Initialize and in exclusive mode that
    // takes the device.
    CHECK(strict.tried == 2);
}

TEST_CASE("a float source that the device refuses reaches it through Path B",
          "[negotiate][path]")
{
    // The measured case. Every lossy decoder here reports F32; the endpoint this
    // was written on will not take float in exclusive mode. Under the default
    // that file does not play, which is honest and useless; under `auto` it
    // plays converted, and the report says so.
    mp::Format source = cd_audio();
    source.sample_type = mp::SampleType::f32;

    mp::test::FakeSinkRules rules;
    rules.accepts = [](const mp::Format& f) { return f.sample_type == mp::SampleType::s32; };

    mp::test::FakeSink refuses_float{rules};
    mp::Sink strict = refuses_float.handle();
    CHECK_FALSE(mp::negotiate_best(strict, source, mp::PathPolicy::bit_exact).ok);

    mp::test::FakeSink again{rules};
    mp::Sink sink = again.handle();
    const auto negotiated = mp::negotiate_best(sink, source, mp::PathPolicy::automatic);
    REQUIRE(negotiated.ok);
    CHECK(negotiated.fidelity == mp::Fidelity::converted);
    CHECK(mp::needs_processing(negotiated.fidelity));
    CHECK(negotiated.accepted.sample_type == mp::SampleType::s32);
}

TEST_CASE("Path B plays a float source a device would not take", "[processed]")
{
    constexpr std::uint32_t period = 64;
    mp::Format source_format = cd_audio();
    source_format.sample_type = mp::SampleType::f32;

    // A ramp, so the conversion can be checked rather than only its length.
    const std::size_t frames = period * 4;
    std::vector<std::uint8_t> bytes(frames * mp::frame_bytes(source_format));
    for (std::size_t i = 0; i < frames * source_format.channels; ++i) {
        const auto v = static_cast<float>(static_cast<double>(i % 200) / 400.0);
        std::memcpy(bytes.data() + i * 4, &v, 4);
    }

    VectorSource source{source_format, bytes};
    mp::test::FakeSinkRules rules;
    rules.period_frames = period;
    rules.accepts = [](const mp::Format& f) { return f.sample_type == mp::SampleType::s32; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    const auto negotiated =
        mp::negotiate_best(sink, source_format, mp::PathPolicy::automatic);
    REQUIRE(negotiated.ok);
    REQUIRE(mp::needs_processing(negotiated.fidelity));

    mp::ConvertConfig conversion;
    conversion.dither = false;
    mp::ProcessedGraph graph{source, sink, negotiated.accepted, period, conversion};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();

    CHECK(graph.error() == MP_OK);
    CHECK(graph.lossy());
    CHECK(graph.stats().underruns == 0);

    // Every frame arrived, and each sample is the float scaled to 32 bits.
    const auto captured = device.captured();
    REQUIRE(captured.size() >= frames * mp::frame_bytes(negotiated.accepted));
    for (std::size_t i = 0; i < frames * source_format.channels; ++i) {
        float in = 0.0F;
        std::memcpy(&in, bytes.data() + i * 4, 4);
        std::int32_t out = 0;
        std::memcpy(&out, captured.data() + i * 4, 4);
        INFO("sample " << i);
        REQUIRE(out == static_cast<std::int32_t>(std::llround(static_cast<double>(in) *
                                                              2147483648.0)));
    }
}

TEST_CASE("Path B refuses what its converter cannot do", "[processed]")
{
    // No resampler and no remix: a graph asked for either says so at start
    // rather than producing something.
    mp::Format wire = cd_audio();
    wire.sample_rate = 96000;

    VectorSource source{cd_audio(), pattern(4096)};
    mp::test::FakeSink device{mp::test::FakeSinkRules{}};
    mp::Sink sink = device.handle();

    mp::ProcessedGraph graph{source, sink, wire, 64};
    CHECK(graph.start() == MP_ERR_INVALID);
}

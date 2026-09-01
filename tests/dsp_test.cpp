// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DSP chain, tested against stages this file writes itself.
//
// The stages are fakes on purpose. A test that loaded `dsp_gain` would be
// testing that module; what has to be right here is the host side -- that the
// formats chain, that a stage which changes the rate is believed and given room
// for what it produces, that a stage holding audio is drained at the end rather
// than truncated, and that a refusal names the stage that refused.

#include "fake_sink.hpp"

#include "mediaperch/dsp.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/processed.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

// The host only ever sees a pointer, so the test is free to say what it points
// at. Nothing else in this binary defines it.
struct MpDsp {
    enum class Kind { gain, upsample2, delay, refuse };

    Kind kind = Kind::gain;
    double gain = 0.5;
    std::uint32_t channels = 0;
    std::uint32_t capacity = 0;
    /// `delay` only: one line per channel, and where in it we are.
    std::vector<std::vector<double>> line;
    std::size_t at = 0;
    std::uint32_t held = 0;
};

namespace {

constexpr std::uint32_t k_delay_frames = 16;

template <MpDsp::Kind K>
MpResult MP_CALL fake_open(MpDsp** out)
{
    *out = new MpDsp{};
    (*out)->kind = K;
    return MP_OK;
}

void MP_CALL fake_close(MpDsp* d)
{
    delete d;
}

MpResult MP_CALL fake_configure(MpDsp* d, const MpFormat* in_format, std::uint32_t max_frames,
                                MpFormat* out_format, std::uint32_t* out_max_frames)
{
    if (d->kind == MpDsp::Kind::refuse) {
        return MP_ERR_FORMAT;
    }
    d->channels = in_format->channels;
    *out_format = *in_format;
    d->capacity = max_frames;
    if (d->kind == MpDsp::Kind::upsample2) {
        out_format->sample_rate = in_format->sample_rate * 2;
        d->capacity = max_frames * 2;
    }
    if (d->kind == MpDsp::Kind::delay) {
        d->line.assign(d->channels, std::vector<double>(k_delay_frames, 0.0));
        d->at = 0;
        d->held = 0;
    }
    *out_max_frames = d->capacity;
    return MP_OK;
}

MpResult MP_CALL fake_process(MpDsp* d, const double* const* in, std::uint32_t in_frames,
                              double* const* out, std::uint32_t out_capacity,
                              std::uint32_t* out_frames)
{
    *out_frames = 0;
    if (in_frames == 0) {
        return MP_OK;
    }
    switch (d->kind) {
    case MpDsp::Kind::gain:
        if (in_frames > out_capacity) {
            return MP_ERR_INVALID;
        }
        for (std::uint32_t c = 0; c < d->channels; ++c) {
            for (std::uint32_t n = 0; n < in_frames; ++n) {
                out[c][n] = in[c][n] * d->gain;
            }
        }
        *out_frames = in_frames;
        return MP_OK;

    case MpDsp::Kind::upsample2:
        // Zero-order hold. Not a resampler; a stage that produces more frames
        // than it was given, which is the only property under test.
        if (in_frames * 2 > out_capacity) {
            return MP_ERR_INVALID;
        }
        for (std::uint32_t c = 0; c < d->channels; ++c) {
            for (std::uint32_t n = 0; n < in_frames; ++n) {
                out[c][2 * n] = in[c][n];
                out[c][2 * n + 1] = in[c][n];
            }
        }
        *out_frames = in_frames * 2;
        return MP_OK;

    case MpDsp::Kind::delay:
        if (in_frames > out_capacity) {
            return MP_ERR_INVALID;
        }
        for (std::uint32_t n = 0; n < in_frames; ++n) {
            const std::size_t at = (d->at + n) % k_delay_frames;
            for (std::uint32_t c = 0; c < d->channels; ++c) {
                out[c][n] = d->line[c][at];
                d->line[c][at] = in[c][n];
            }
        }
        d->at = (d->at + in_frames) % k_delay_frames;
        d->held = std::min<std::uint32_t>(d->held + in_frames, k_delay_frames);
        *out_frames = in_frames;
        return MP_OK;

    case MpDsp::Kind::refuse:
        break;
    }
    return MP_ERR_INVALID;
}

MpResult MP_CALL fake_flush(MpDsp* d, double* const* out, std::uint32_t out_capacity,
                            std::uint32_t* out_frames)
{
    *out_frames = 0;
    if (d->kind != MpDsp::Kind::delay || d->held == 0) {
        return MP_OK;
    }
    const std::uint32_t n = std::min(d->held, out_capacity);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t at = (d->at + i) % k_delay_frames;
        for (std::uint32_t c = 0; c < d->channels; ++c) {
            out[c][i] = d->line[c][at];
        }
    }
    d->at = (d->at + n) % k_delay_frames;
    d->held -= n;
    *out_frames = n;
    return MP_OK;
}

MpResult MP_CALL fake_set(MpDsp* d, const char* key, const char* value)
{
    if (std::strcmp(key, "gain") != 0) {
        return MP_ERR_UNSUPPORTED;
    }
    d->gain = std::strtod(value, nullptr);
    return MP_OK;
}

MpResult MP_CALL fake_describe(MpDsp* d, std::uint32_t index, char* out,
                               std::uint32_t out_bytes)
{
    if (index != 0) {
        return MP_END;
    }
    std::snprintf(out, out_bytes, "gain\t%.3f\thow much of it there is", d->gain);
    return MP_OK;
}

template <MpDsp::Kind K>
const MpDspVtbl& fake_vtbl()
{
    static const MpDspVtbl vtbl{sizeof(MpDspVtbl),
                                0,
                                &fake_open<K>,
                                &fake_close,
                                &fake_configure,
                                &fake_process,
                                &fake_flush,
                                &fake_set,
                                &fake_describe};
    return vtbl;
}

mp::Format cd_audio_f64()
{
    return mp::Format{.sample_rate = 44100,
                      .channels = 2,
                      .channel_mask = 0,
                      .sample_type = mp::SampleType::f64,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = 0};
}

/// Interleaved doubles, distinguishable per channel and per frame.
std::vector<double> ramp(std::uint32_t frames, std::uint32_t channels)
{
    std::vector<double> out(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t n = 0; n < frames; ++n) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[static_cast<std::size_t>(n) * channels + c] =
                0.25 * static_cast<double>(n % 17) + 0.01 * static_cast<double>(c);
        }
    }
    return out;
}

/// Everything a chain gives back for one block, plus everything it still held.
std::vector<double> run_and_drain(mp::DspChain& chain, const std::vector<double>& in,
                                  std::uint32_t frames)
{
    std::vector<double> out;
    std::vector<double> block;
    std::uint32_t produced = 0;
    REQUIRE(chain.run(in.data(), frames, block, produced));
    out.insert(out.end(), block.begin(),
               block.begin() + static_cast<std::ptrdiff_t>(
                                   produced * chain.output_format().channels));

    for (int round = 0; round < 64 && !chain.flush_done(); ++round) {
        REQUIRE(chain.flush(block, produced));
        out.insert(out.end(), block.begin(),
                   block.begin() + static_cast<std::ptrdiff_t>(
                                       produced * chain.output_format().channels));
    }
    CHECK(chain.flush_done());
    return out;
}

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
        const std::size_t n = std::min(bytes - (bytes % stride), data_.size() - offset_);
        std::memcpy(dst, data_.data() + offset_, n);
        offset_ += n;
        return n;
    }

private:
    mp::Format format_;
    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
};

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

TEST_CASE("an empty chain is a copy, and says the format is unchanged", "[dsp]")
{
    mp::DspChain chain;
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 128, why));
    CHECK(chain.output_format() == cd_audio_f64());
    CHECK(chain.output_capacity() == 128);

    const auto in = ramp(128, 2);
    std::vector<double> out;
    std::uint32_t produced = 0;
    REQUIRE(chain.run(in.data(), 128, out, produced));
    CHECK(produced == 128);
    CHECK(std::equal(in.begin(), in.end(), out.begin()));
}

TEST_CASE("a stage sees planes and the chain hands back interleaved", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "gain");
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 64, why));

    const auto in = ramp(64, 2);
    const auto out = run_and_drain(chain, in, 64);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        INFO("sample " << i);
        // Deinterleave and interleave are inverses, which is only visible if
        // the channels differ -- they do, by 0.01.
        REQUIRE(out[i] == Catch::Approx(in[i] * 0.5));
    }
}

TEST_CASE("stages run in the order they were added", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "first");
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "second");
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 32, why));
    REQUIRE(chain.size() == 2);
    REQUIRE(chain.at(1).set("gain", "0.25") == MP_OK);

    const auto in = ramp(32, 2);
    const auto out = run_and_drain(chain, in, 32);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Catch::Approx(in[i] * 0.5 * 0.25));
    }
}

TEST_CASE("a stage that changes the rate is believed, and given room", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::upsample2>(), "up");
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 64, why));

    // The format the device will have to be asked for is this one, not the
    // source's -- which is the whole reason `configure` answers with a format.
    CHECK(chain.output_format().sample_rate == 88200);
    CHECK(chain.output_capacity() == 128);

    const auto in = ramp(64, 2);
    const auto out = run_and_drain(chain, in, 64);
    REQUIRE(out.size() == in.size() * 2);
    for (std::size_t n = 0; n < 64; ++n) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            REQUIRE(out[(2 * n) * 2 + c] == Catch::Approx(in[n * 2 + c]));
            REQUIRE(out[(2 * n + 1) * 2 + c] == Catch::Approx(in[n * 2 + c]));
        }
    }
}

TEST_CASE("what a stage still holds at the end of the stream comes out", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::delay>(), "delay");
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 128, why));

    const auto in = ramp(128, 2);
    const auto out = run_and_drain(chain, in, 128);

    // A delay makes the point plainly: without the drain the last 16 frames of
    // the file are simply missing, and nothing else would have noticed.
    REQUIRE(out.size() == in.size() + k_delay_frames * 2);
    for (std::size_t n = 0; n < k_delay_frames; ++n) {
        CHECK(out[n * 2] == 0.0);
    }
    for (std::size_t n = 0; n < 128; ++n) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            INFO("frame " << n);
            REQUIRE(out[(n + k_delay_frames) * 2 + c] == Catch::Approx(in[n * 2 + c]));
        }
    }
}

TEST_CASE("every stage is drained, not only the first", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::delay>(), "one");
    chain.add(fake_vtbl<MpDsp::Kind::delay>(), "two");
    std::string why;
    REQUIRE(chain.configure(cd_audio_f64(), 128, why));

    const auto in = ramp(128, 2);
    const auto out = run_and_drain(chain, in, 128);
    REQUIRE(out.size() == in.size() + 2 * k_delay_frames * 2);
    for (std::size_t n = 0; n < 128; ++n) {
        INFO("frame " << n);
        REQUIRE(out[(n + 2 * k_delay_frames) * 2] == Catch::Approx(in[n * 2]));
    }
}

TEST_CASE("a chain that will not configure names the stage that refused", "[dsp]")
{
    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "innocent");
    chain.add(fake_vtbl<MpDsp::Kind::refuse>(), "guilty");
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "also innocent");

    std::string why;
    REQUIRE_FALSE(chain.configure(cd_audio_f64(), 64, why));
    CHECK(why.find("guilty") != std::string::npos);
}

TEST_CASE("a stage describes itself, so a host can offer what it did not know about",
          "[dsp]")
{
    mp::DspStage stage{fake_vtbl<MpDsp::Kind::gain>(), "gain"};
    REQUIRE(stage.open());
    const auto lines = stage.describe();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].rfind("gain\t", 0) == 0);
    CHECK(stage.set("nonsense", "1") == MP_ERR_UNSUPPORTED);
}

TEST_CASE("Path B runs the chain, and quantises once at the end of it", "[dsp][processed]")
{
    constexpr std::uint32_t period = 64;
    constexpr std::uint32_t frames = period * 4;

    mp::Format source_format = cd_audio_f64();
    source_format.sample_type = mp::SampleType::s16;

    // A quiet ramp, so halving it stays well inside the container and the
    // arithmetic is checkable exactly.
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(200 * static_cast<int>(i % 40));
    }
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());

    VectorSource source{source_format, bytes};
    mp::test::FakeSinkRules rules;
    rules.period_frames = period;
    rules.accepts = [](const mp::Format& f) { return f.sample_type == mp::SampleType::s16; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "half");
    std::string why;
    REQUIRE(chain.configure(mp::dsp_bus_format(source_format), period, why));

    // Negotiating for real, because the device is what decides the wire format
    // and because a chain is what the device is then offered.
    const auto negotiated =
        mp::negotiate_best(sink, chain.output_format(), mp::PathPolicy::processed);
    REQUIRE(negotiated.ok);
    REQUIRE(negotiated.accepted.sample_type == mp::SampleType::s16);

    mp::ConvertConfig conversion;
    conversion.dither = mp::DitherKind::none; // an exact answer is wanted here
    mp::ProcessedGraph graph{source,     sink,
                             negotiated.accepted, period,
                             conversion, nullptr,
                             mp::PassthroughConfig{},
                             &chain};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();
    CHECK(graph.error() == MP_OK);
    CHECK(graph.stats().underruns == 0);

    const auto captured = device.captured();
    REQUIRE(captured.size() >= bytes.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::int16_t out = 0;
        std::memcpy(&out, captured.data() + i * 2, 2);
        INFO("sample " << i);
        // Halved on the f64 bus and rounded once, at the wire. 200 * k is
        // even, so the half is exact and this is an equality, not a bound.
        REQUIRE(out == samples[i] / 2);
    }
}

TEST_CASE("Path B writes the chain's tail, not just what the source gave it",
          "[dsp][processed]")
{
    constexpr std::uint32_t period = 64;
    constexpr std::uint32_t frames = period * 4;

    mp::Format source_format = cd_audio_f64();
    source_format.sample_type = mp::SampleType::s16;

    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(100 + 7 * static_cast<int>(i % 90));
    }
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());

    VectorSource source{source_format, bytes};
    mp::test::FakeSinkRules rules;
    rules.period_frames = period;
    rules.accepts = [](const mp::Format& f) { return f.sample_type == mp::SampleType::s16; };
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();

    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::delay>(), "delay");
    std::string why;
    REQUIRE(chain.configure(mp::dsp_bus_format(source_format), period, why));

    const auto negotiated =
        mp::negotiate_best(sink, chain.output_format(), mp::PathPolicy::processed);
    REQUIRE(negotiated.ok);

    mp::ConvertConfig conversion;
    conversion.dither = mp::DitherKind::none;
    mp::ProcessedGraph graph{source,     sink,
                             negotiated.accepted, period,
                             conversion, nullptr,
                             mp::PassthroughConfig{},
                             &chain};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();
    CHECK(graph.error() == MP_OK);

    // Every input frame is present, 16 frames later than it went in. The last
    // 16 of them exist only because the chain was drained: without that they
    // are silently dropped, and a resampler would lose the end of every file.
    const auto captured = device.captured();
    REQUIRE(captured.size() >= (frames + k_delay_frames) * 4);
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            std::int16_t out = 0;
            std::memcpy(&out, captured.data() + ((n + k_delay_frames) * 2 + c) * 2, 2);
            INFO("frame " << n << " channel " << c);
            REQUIRE(out == samples[n * 2 + c]);
        }
    }
}

TEST_CASE("a chain configured for something else is refused, not run", "[dsp][processed]")
{
    mp::Format source_format = cd_audio_f64();
    source_format.sample_type = mp::SampleType::s16;

    VectorSource source{source_format, std::vector<std::uint8_t>(4096, 0)};
    mp::test::FakeSink device{mp::test::FakeSinkRules{}};
    mp::Sink sink = device.handle();

    mp::DspChain chain;
    chain.add(fake_vtbl<MpDsp::Kind::gain>(), "half");
    std::string why;
    mp::Format wrong = mp::dsp_bus_format(source_format);
    wrong.channels = 1;
    REQUIRE(chain.configure(wrong, 64, why));

    mp::ProcessedGraph graph{source,          sink, source_format, 64, {}, nullptr,
                             mp::PassthroughConfig{}, &chain};
    CHECK(graph.start() == MP_ERR_INVALID);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keeping the sound flowing: a queue that does not stop between tracks, and a
// transport that can pause and seek without the device noticing.
//
// Every test here is against the fake sink, which records every byte handed to
// `ReleaseBuffer`. That matters more for transport than for anything else in
// this tree: "gapless" and "the seek landed where I asked" are claims about
// what reached the device, and the only way to check a claim about that is to
// look at what reached the device.

#include "fake_sink.hpp"

#include "mediaperch/passthrough.hpp"
#include "mediaperch/queue.hpp"
#include "mediaperch/sink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
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

/// A source that can seek, because transport is what this file is about.
class Tape final : public mp::ISource {
public:
    Tape(const mp::Format& format, std::vector<std::uint8_t> data)
        : format_(format), data_(std::move(data))
    {
    }

    [[nodiscard]] const mp::Format& format() const noexcept override { return format_; }

    std::size_t read(void* dst, std::size_t bytes) override
    {
        const std::size_t stride = mp::frame_bytes(format_);
        const std::size_t n = std::min(bytes - (bytes % stride), data_.size() - at_);
        std::memcpy(dst, data_.data() + at_, n);
        at_ += n;
        return n;
    }

    [[nodiscard]] bool seekable() const noexcept override { return true; }
    bool seek(std::uint64_t frame) override
    {
        const std::size_t stride = mp::frame_bytes(format_);
        const std::size_t offset = static_cast<std::size_t>(frame) * stride;
        if (offset > data_.size()) {
            return false;
        }
        at_ = offset;
        return true;
    }
    [[nodiscard]] std::uint64_t length_frames() const noexcept override
    {
        return data_.size() / mp::frame_bytes(format_);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }

private:
    mp::Format format_;
    std::vector<std::uint8_t> data_;
    std::size_t at_ = 0;
};

/// Distinguishable bytes: a ramp would survive an off-by-one shift unnoticed,
/// and an off-by-one shift is exactly what a bad seek is.
std::vector<std::uint8_t> pattern(std::size_t bytes, std::uint8_t seed)
{
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37u + seed * 101u + 11u) & 0xFFu);
    }
    return out;
}

/// A playlist over sources the test already holds.
class Fixed final : public mp::IPlaylist {
public:
    explicit Fixed(std::vector<mp::ISource*> items) : items_(std::move(items)) {}
    mp::ISource* at(std::size_t index) override
    {
        return index < items_.size() ? items_[index] : nullptr;
    }
    [[nodiscard]] std::size_t opened() const noexcept { return highest_ + 1; }

private:
    std::vector<mp::ISource*> items_;
    std::size_t highest_ = 0;
};

/// Polls until something is true, because a fixed sleep in a test about threads
/// is a test that passes on the machine it was written on.
template <typename Predicate>
bool wait_for(Predicate ready, int milliseconds = 2000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

template <typename Graph>
bool wait_until_stopped(const Graph& graph, int seconds = 5)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
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
// The queue
// --------------------------------------------------------------------------

TEST_CASE("a queue joins two tracks with nothing in between", "[transport][queue]")
{
    // The claim is byte-for-byte, and it is checked byte-for-byte: what the
    // device received is the first track followed immediately by the second,
    // with no silence, no repeat and no gap.
    const auto first = pattern(4096, 1);
    const auto second = pattern(3072, 2);
    Tape a{cd_audio(), first};
    Tape b{cd_audio(), second};
    Fixed playlist{{&a, &b}};

    mp::Queue queue{playlist};
    std::string why;
    INFO(why);
    REQUIRE(queue.open(why));

    mp::test::FakeSinkRules rules;
    rules.period_frames = 64;
    // A device with a clock. Transport is a claim about a stream that keeps up,
    // and a render thread with nothing to wait for outruns the decode thread by
    // a factor of thousands.
    rules.pace_us = 1000;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();
    const auto negotiated = mp::negotiate_best(sink, cd_audio(), mp::PathPolicy::bit_exact);
    REQUIRE(negotiated.ok);

    mp::PassthroughGraph graph{queue, sink, negotiated.accepted, 64, negotiated.fidelity};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_until_stopped(graph));
    graph.stop();

    CHECK(graph.error() == MP_OK);
    CHECK(graph.stats().underruns == 0);
    CHECK(queue.completed() == 2);

    const auto captured = device.captured();
    REQUIRE(captured.size() >= first.size() + second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO("first track, byte " << i);
        REQUIRE(captured[i] == first[i]);
    }
    for (std::size_t i = 0; i < second.size(); ++i) {
        INFO("second track, byte " << i);
        REQUIRE(captured[first.size() + i] == second[i]);
    }
}

TEST_CASE("a queue will not join two formats, and says which", "[transport][queue]")
{
    // 44.1 kHz followed by 96 kHz cannot share a device stream in exclusive
    // mode, so joining them would mean converting one without being asked.
    mp::Format other = cd_audio();
    other.sample_rate = 96000;

    Tape a{cd_audio(), pattern(2048, 3)};
    Tape b{other, pattern(2048, 4)};
    Fixed playlist{{&a, &b}};

    mp::Queue queue{playlist};
    std::string why;
    REQUIRE(queue.open(why));

    std::vector<std::uint8_t> sink_buffer(8192);
    std::size_t total = 0;
    for (;;) {
        const std::size_t got = queue.read(sink_buffer.data(), sink_buffer.size());
        if (got == 0) {
            break;
        }
        total += got;
    }
    CHECK(total == 2048); // the first track, and not a byte of the second
    CHECK(queue.stopped() == mp::QueueStop::format_change);
    CHECK(queue.next_format().sample_rate == 96000);
    CHECK(queue.index() == 0); // still on the one it played
}

TEST_CASE("skip abandons the rest of a track and the device does not notice",
          "[transport][queue]")
{
    const auto first = pattern(8192, 5);
    const auto second = pattern(1024, 6);
    Tape a{cd_audio(), first};
    Tape b{cd_audio(), second};
    Fixed playlist{{&a, &b}};

    mp::Queue queue{playlist};
    std::string why;
    REQUIRE(queue.open(why));

    std::vector<std::uint8_t> out(512);
    REQUIRE(queue.read(out.data(), out.size()) == 512);
    queue.skip();

    // The next read comes from the second track, from its start.
    std::vector<std::uint8_t> after(second.size());
    std::size_t filled = 0;
    while (filled < after.size()) {
        const std::size_t got = queue.read(after.data() + filled, after.size() - filled);
        if (got == 0) {
            break;
        }
        filled += got;
    }
    REQUIRE(filled == second.size());
    CHECK(std::equal(after.begin(), after.end(), second.begin()));
    CHECK(queue.index() == 1);
}

// --------------------------------------------------------------------------
// Pause and seek
// --------------------------------------------------------------------------

TEST_CASE("pausing stops the device and resuming carries on", "[transport]")
{
    Tape tape{cd_audio(), pattern(44100 * 4, 7)}; // a second or so
    mp::test::FakeSinkRules rules;
    rules.period_frames = 64;
    // A device with a clock. Transport is a claim about a stream that keeps up,
    // and a render thread with nothing to wait for outruns the decode thread by
    // a factor of thousands.
    rules.pace_us = 1000;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();
    const auto negotiated = mp::negotiate_best(sink, cd_audio(), mp::PathPolicy::bit_exact);
    REQUIRE(negotiated.ok);

    mp::PassthroughGraph graph{tape, sink, negotiated.accepted, 64, negotiated.fidelity};
    REQUIRE(graph.start() == MP_OK);

    // Let it play, then pause and check that it stops -- both that the device
    // was told and that nothing more arrives at it.
    REQUIRE(wait_for([&] { return device.captured_size() > 0; }));
    graph.pause();
    CHECK(graph.paused());
    REQUIRE(wait_for([&] { return !device.started(); }));

    // Whatever was already in flight lands, and then nothing more does.
    std::size_t at_pause = device.captured_size();
    REQUIRE(wait_for([&] {
        const std::size_t now = device.captured_size();
        const bool settled = now == at_pause;
        at_pause = now;
        return settled;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK(device.captured_size() == at_pause);
    CHECK_FALSE(device.started());

    // And resuming starts it again, from where it stopped rather than from a
    // ring that has been thrown away.
    graph.resume();
    CHECK_FALSE(graph.paused());
    INFO("error " << graph.error() << ", running " << graph.running());
    REQUIRE(wait_for([&] { return device.captured_size() > at_pause; }));
    CHECK(device.started());

    graph.stop();
    CHECK(graph.error() == MP_OK);

    // Everything the device received is the file, in order, with the pause
    // adding nothing at all.
    const auto captured = device.captured();
    const std::size_t compare = std::min(captured.size(), tape.data().size());
    for (std::size_t i = 0; i < compare; ++i) {
        INFO("byte " << i);
        REQUIRE(captured[i] == tape.data()[i]);
    }
}

TEST_CASE("a seek lands where it was asked to", "[transport]")
{
    const auto data = pattern(44100 * 4, 9);
    Tape tape{cd_audio(), data};
    mp::test::FakeSinkRules rules;
    rules.period_frames = 64;
    // A device with a clock. Transport is a claim about a stream that keeps up,
    // and a render thread with nothing to wait for outruns the decode thread by
    // a factor of thousands.
    rules.pace_us = 1000;
    mp::test::FakeSink device{rules};
    mp::Sink sink = device.handle();
    const auto negotiated = mp::negotiate_best(sink, cd_audio(), mp::PathPolicy::bit_exact);
    REQUIRE(negotiated.ok);

    mp::PassthroughGraph graph{tape, sink, negotiated.accepted, 64, negotiated.fidelity};
    REQUIRE(graph.start() == MP_OK);
    REQUIRE(wait_for([&] { return device.captured_size() > 0; }));

    constexpr std::uint64_t k_target = 8000;
    const std::size_t before = device.captured_size();
    REQUIRE(graph.seek(k_target));
    // The position is the device's, not the decoder's: right after a seek it is
    // the frame that was asked for plus however much has played since, which is
    // a few periods rather than the ring's worth the decoder is ahead by.
    const std::uint64_t at = graph.position_frames();
    INFO("position after the seek: " << at);
    CHECK(at >= k_target);
    CHECK(at < k_target + 44100 / 4);

    const std::size_t after_seek = device.captured_size();
    REQUIRE(wait_for([&] { return device.captured_size() > after_seek + 4096; }));
    graph.stop();
    CHECK(graph.error() == MP_OK);

    // Somewhere after the seek, the device received the file from frame 8000 --
    // and it is a byte-for-byte run, not a resemblance.
    const auto captured = device.captured();
    const std::size_t stride = mp::frame_bytes(cd_audio());
    const std::size_t from = static_cast<std::size_t>(k_target) * stride;
    REQUIRE(captured.size() > before);
    const auto needle_begin = data.begin() + static_cast<std::ptrdiff_t>(from);
    const auto found = std::search(captured.begin() + static_cast<std::ptrdiff_t>(before),
                                   captured.end(), needle_begin,
                                   needle_begin + 512);
    INFO("looked for frame " << k_target << " past byte " << before << " of "
                             << captured.size());
    CHECK(found != captured.end());
}

TEST_CASE("a source that cannot seek says so rather than pretending", "[transport]")
{
    // The tone generator has no position to go to, and a graph over one must
    // not report a successful seek that did nothing.
    class Endless final : public mp::ISource {
    public:
        [[nodiscard]] const mp::Format& format() const noexcept override
        {
            return format_;
        }
        std::size_t read(void* dst, std::size_t bytes) override
        {
            std::memset(dst, 0, bytes);
            return bytes;
        }

    private:
        mp::Format format_ = cd_audio();
    } endless;

    mp::test::FakeSink device{mp::test::FakeSinkRules{}};
    mp::Sink sink = device.handle();
    const auto negotiated = mp::negotiate_best(sink, cd_audio(), mp::PathPolicy::bit_exact);
    REQUIRE(negotiated.ok);

    mp::PassthroughGraph graph{endless, sink, negotiated.accepted, 64, negotiated.fidelity};
    CHECK_FALSE(graph.seek(1000));
    CHECK(graph.position_frames() == 0);
}

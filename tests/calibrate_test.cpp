// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sweep, against a machine that does not exist.
//
// The one thing a calibration cannot fake is the run -- a device with a real
// deadline is the whole measurement. Everything *around* the run is arithmetic,
// and this is that: which sizes to try, in which direction, when to stop, and
// what a pile of runs adds up to. The fake host below is the second
// implementation §15 asks for before an interface exists.

#include "mediaperch/calibrate.hpp"
#include "mediaperch/profile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

/// A machine whose ring has to be at least `needs` periods, and which drops
/// frames in inverse proportion to the threads it is given.
class FakeMachine final : public mp::ICalibrationHost {
public:
    explicit FakeMachine(std::uint32_t needs) : needs_(needs) {}

    bool inspect(const std::string& file, mp::StreamShape& shape, double& duration,
                 std::string& why) override
    {
        if (file == "no.mkv") {
            why = "nothing here decodes it";
            return false;
        }
        shape = mp::StreamShape{MP_CODEC_HEVC, 3840, 2160, 24000, 1001};
        duration = duration_;
        return true;
    }

    bool play(const mp::CalibrationRun& run, mp::RunResult& result, std::string& why) override
    {
        (void)why;
        tried.push_back(run);
        result.ok = true;
        result.period_frames = 144;
        result.sample_rate = 48000;
        result.frame_bytes = 4;
        std::uint32_t capacity = 1;
        while (capacity < run.ring_periods) {
            capacity *= 2;
        }
        result.ring_bytes = static_cast<std::size_t>(capacity) * 144 * 4;
        result.underruns = run.ring_periods < needs_ ? 3 : 0;
        result.low_water_bytes = result.underruns != 0 ? 0 : 288 * 4;
        const std::uint32_t threads = run.decoder_threads == 0 ? 1 : run.decoder_threads;
        result.frames_dropped = 64 / threads;
        return true;
    }

    void say(const std::string&) override { ++said; }

    /// The ring sizes it was asked to play, in the order it was asked, ignoring
    /// which window: what the sweep's path through the sizes actually was.
    [[nodiscard]] std::vector<std::uint32_t> path() const
    {
        std::vector<std::uint32_t> out;
        for (const auto& run : tried) {
            if (out.empty() || out.back() != run.ring_periods) {
                out.push_back(run.ring_periods);
            }
        }
        return out;
    }

    std::vector<mp::CalibrationRun> tried;
    int said = 0;

private:
    std::uint32_t needs_;
    double duration_ = 300.0;
};

mp::CalibrationPlan plan_for(mp::Sweep sweep, std::uint32_t start)
{
    mp::CalibrationPlan plan;
    plan.dimensions = mp::Dimension::ring;
    plan.sweep = sweep;
    plan.windows = mp::Windows{1, 10.0};
    plan.start_ring = start;
    plan.highest_ring = 1024;
    return plan;
}

} // namespace

TEST_CASE("the sweep starts where it was told to start", "[calibrate]")
{
    // **This is the whole of the bug this test exists for.** An earlier version
    // built every size from one to the ceiling and sorted them descending, so
    // it began at the ceiling whatever it was told -- nine real-time runs
    // before reaching a plausible answer, on every file.
    FakeMachine machine{32};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    REQUIRE(report.profile.measured.size() == 1);
    REQUIRE_FALSE(machine.path().empty());
    CHECK(machine.path().front() == 128);
}

TEST_CASE("a start that holds is walked downwards", "[calibrate]")
{
    FakeMachine machine{32};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    // 128 held, so smaller sizes are worth trying: 64, 32, then 16 which fails
    // and ends it. The answer is 32, doubled for margin: 96 ms becomes 192.
    CHECK(machine.path() == std::vector<std::uint32_t>{128, 64, 32, 16});
    REQUIRE(report.profile.measured.size() == 1);
    CHECK(report.profile.measured[0].ring_ms == 192.0);
}

TEST_CASE("a start that underruns is walked upwards", "[calibrate]")
{
    // The question this answers: the default underran, so what happens?
    FakeMachine machine{512};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    // Up from the start until one holds, and nothing below it is tried: it
    // already failed at a larger size.
    CHECK(machine.path() == std::vector<std::uint32_t>{128, 256, 512});
    REQUIRE(report.profile.measured.size() == 1);
    // 512 periods is 1536 ms, doubled is 3072 -- far above the default, which
    // is the case a default cannot answer and nothing here clamps.
    CHECK(report.profile.measured[0].ring_ms == 3072.0);
}

TEST_CASE("shrink refuses to grow, and says so", "[calibrate]")
{
    FakeMachine machine{512};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::shrink, 128));
    // It was told only ever smaller. 128 underran, so there is no answer below
    // it either, and the file is skipped rather than quietly grown into range.
    CHECK(machine.path() == std::vector<std::uint32_t>{128});
    CHECK(report.profile.measured.empty());
    REQUIRE(report.skipped.size() == 1);
    CHECK(report.skipped[0].find("without an underrun") != std::string::npos);
}

TEST_CASE("grow never answers below where it started", "[calibrate]")
{
    FakeMachine machine{32};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::grow, 128));
    // 128 holds and that is the answer: grow was asked not to spend runs
    // proving again that smaller rings are smaller.
    CHECK(machine.path() == std::vector<std::uint32_t>{128});
    REQUIRE(report.profile.measured.size() == 1);
    // 128 periods is 384 ms, doubled is 768.
    CHECK(report.profile.measured[0].ring_ms == 768.0);
}

TEST_CASE("every tries every size and does not stop early", "[calibrate]")
{
    FakeMachine machine{32};
    auto plan = plan_for(mp::Sweep::every, 128);
    plan.lowest_ring = 16;
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan);
    CHECK(machine.path() == std::vector<std::uint32_t>{1024, 512, 256, 128, 64, 32, 16});
    REQUIRE(report.profile.measured.size() == 1);
    CHECK(report.profile.measured[0].ring_ms == 192.0);
}

TEST_CASE("the order a sweep will take can be asked for before it runs",
          "[calibrate]")
{
    auto plan = plan_for(mp::Sweep::shrink, 128);
    plan.lowest_ring = 16;
    CHECK(mp::ring_order(plan) == std::vector<std::uint32_t>{128, 64, 32, 16});

    plan.sweep = mp::Sweep::grow;
    plan.highest_ring = 512;
    CHECK(mp::ring_order(plan) == std::vector<std::uint32_t>{128, 256, 512});

    // Adaptive knows only its first size. Saying so beats inventing an order
    // that the second run may contradict.
    plan.sweep = mp::Sweep::adaptive;
    CHECK(mp::ring_order(plan) == std::vector<std::uint32_t>{128});

    // Powers of two throughout, because the sizes in between are the same ring.
    plan.sweep = mp::Sweep::every;
    for (const std::uint32_t size : mp::ring_order(plan)) {
        CHECK((size & (size - 1)) == 0);
    }
}

TEST_CASE("a start nobody rounded lands on a size the ring can be", "[calibrate]")
{
    auto plan = plan_for(mp::Sweep::shrink, 100);
    plan.lowest_ring = 16;
    // 100 periods is a 64-period ring, so that is where the sweep starts: the
    // sizes in between are not distinguishable and trying them is trying one
    // size three times.
    CHECK(mp::ring_order(plan) == std::vector<std::uint32_t>{64, 32, 16});
}

TEST_CASE("the ceiling and the floor are where the sweep gives up", "[calibrate]")
{
    FakeMachine machine{4096};
    auto plan = plan_for(mp::Sweep::adaptive, 128);
    plan.highest_ring = 512;
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan);
    CHECK(machine.path() == std::vector<std::uint32_t>{128, 256, 512});
    CHECK(report.profile.measured.empty());
    REQUIRE(report.skipped.size() == 1);
}

TEST_CASE("windows are the beginning, the middle and the run-out", "[calibrate]")
{
    const mp::Windows three{3, 10.0};
    const auto starts = mp::window_starts(three, 130.0);
    REQUIRE(starts.size() == 3);
    CHECK(starts[0] == 0.0);
    CHECK(starts[1] == 60.0);
    // The last window ends where the file does.
    CHECK(starts[2] == 120.0);
}

TEST_CASE("a file too short for the plan is measured once, not three times",
          "[calibrate]")
{
    const mp::Windows three{3, 10.0};
    const auto starts = mp::window_starts(three, 4.0);
    REQUIRE(starts.size() == 1);
    CHECK(starts[0] == 0.0);

    CHECK(mp::window_starts(mp::Windows{0, 10.0}, 100.0).empty());
    CHECK(mp::window_starts(mp::Windows{3, 0.0}, 100.0).empty());
}

TEST_CASE("every window has to hold, not the average of them", "[calibrate]")
{
    FakeMachine machine{32};
    auto plan = plan_for(mp::Sweep::adaptive, 128);
    plan.windows = mp::Windows{3, 10.0};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan);
    REQUIRE(report.profile.measured.size() == 1);
    const auto at_16 = std::count_if(machine.tried.begin(), machine.tried.end(),
                                     [](const auto& run) { return run.ring_periods == 16; });
    // One window was enough to disqualify it; the other two were not played.
    CHECK(at_16 == 1);
    const auto at_32 = std::count_if(machine.tried.begin(), machine.tried.end(),
                                     [](const auto& run) { return run.ring_periods == 32; });
    CHECK(at_32 == 3);
}

TEST_CASE("a file nothing can play is skipped and named, not fatal", "[calibrate]")
{
    FakeMachine machine{32};
    const auto report =
        mp::calibrate(machine, {"no.mkv", "a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    CHECK(report.profile.measured.size() == 1);
    REQUIRE(report.skipped.size() == 1);
    CHECK(report.skipped[0].find("no.mkv") != std::string::npos);
}

TEST_CASE("a dimension nobody asked for is not measured", "[calibrate]")
{
    FakeMachine machine{32};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    REQUIRE(report.profile.measured.size() == 1);
    // Threads were not named, so nothing was written down about them, rather
    // than a zero that reads as an answer.
    CHECK(report.profile.measured[0].decoder_threads == 0);

    auto nothing = plan_for(mp::Sweep::adaptive, 128);
    nothing.dimensions = mp::Dimension::none;
    const auto quiet = mp::calibrate(machine, {"a.mkv"}, nothing);
    CHECK(quiet.profile.measured.empty());
    CHECK(quiet.runs == 0);
}

TEST_CASE("threads are measured at the ring the sweep settled on", "[calibrate]")
{
    FakeMachine machine{32};
    auto plan = plan_for(mp::Sweep::adaptive, 128);
    plan.dimensions = mp::Dimension::ring | mp::Dimension::decoder_threads;
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan);
    REQUIRE(report.profile.measured.size() == 1);
    CHECK(report.profile.measured[0].decoder_threads > 0);
    CHECK(report.profile.measured[0].ring_ms == 192.0);
    // 32, not the 128 it started from: a thread count measured at a ring
    // nothing will run at is a thread count measured for somebody else.
    const bool at_the_settled_ring =
        std::any_of(machine.tried.begin(), machine.tried.end(), [](const auto& run) {
            return run.decoder_threads != 0 && run.ring_periods == 32;
        });
    CHECK(at_the_settled_ring);
    const bool none_at_the_start =
        std::none_of(machine.tried.begin(), machine.tried.end(), [](const auto& run) {
            return run.decoder_threads != 0 && run.ring_periods == 128;
        });
    CHECK(none_at_the_start);
}

TEST_CASE("the sweeps a user can name are the ones that exist", "[calibrate]")
{
    mp::Sweep sweep = mp::Sweep::every;
    REQUIRE(mp::sweep_from_name("adaptive", sweep));
    CHECK(sweep == mp::Sweep::adaptive);
    REQUIRE(mp::sweep_from_name("grow", sweep));
    CHECK(sweep == mp::Sweep::grow);
    CHECK_FALSE(mp::sweep_from_name("sideways", sweep));
    CHECK(mp::sweep_name(mp::Sweep::shrink) == "shrink");
}

TEST_CASE("what a calibration decides can be written down and read back",
          "[calibrate]")
{
    FakeMachine machine{32};
    const auto report = mp::calibrate(machine, {"a.mkv"}, plan_for(mp::Sweep::adaptive, 128));
    const std::string text = mp::write_profile(report.profile);
    const auto read = mp::parse_profile(text, "profile.ini");
    CHECK(read.complaints.empty());
    REQUIRE(read.profile.measured.size() == 1);
    CHECK(read.profile.measured[0].ring_ms == 192.0);
    CHECK(read.profile.measured[0].file == "a.mkv");

    // And it answers for the class it measured: 192 ms is 64 periods at 3 ms.
    CHECK(mp::ring_for(read.profile.measured[0].shape, read.profile, 144, 48000, 128) == 64);
}

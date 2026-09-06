// SPDX-License-Identifier: GPL-3.0-or-later
//
// **The driver: which runs to make, and what they add up to.**
//
// A calibration is a sweep. The sweep's shape -- which ring sizes are worth
// trying, in which order, where in a file to try them, when to stop, and how a
// pile of runs becomes one number per class of stream -- is arithmetic and
// belongs where every head can reach it. That is here.
//
// **Every knob in it is a knob**, because every one of them costs real time. A
// calibration cannot run faster than the material: the quantity is whether the
// decode thread beats the device's deadline, and a run with no deadline
// measures nothing. Where to start, which way to walk, how far to walk, how
// many places in a file and for how long -- each of those is minutes of
// somebody's afternoon, and none of them is a question this program can answer
// for them.
//
// **Making one run is not here, and the reason is structural.** What the
// measurement needs is the whole A/V graph: a router reading one file for two
// consumers (§4), a video decoder saturating the machine, and an audio graph
// against a real device with a real deadline. Today that assembly exists in one
// place, `show` in `src/win/mediaperch/main.cpp`, and it is 438 lines of window,
// Direct3D, presenter and refresh switching. So one run arrives through
// `ICalibrationHost`, the same way `Player` gets a file opened and an endpoint
// opened through `IEngineHost` -- and for the same reason: what genuinely
// belongs to a platform comes in through a door, and everything else is written
// once.
//
// §15 says no interface until the second implementation. There are two: the
// head's, and the fake the tests drive this with. That is exactly the
// justification `IEngineHost` has, and `player_test.cpp` says so out loud.

#ifndef MEDIAPERCH_CALIBRATE_HPP
#define MEDIAPERCH_CALIBRATE_HPP

#include "mediaperch/buffering.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// **Which way the sweep walks, and how far.**
enum class Sweep : std::uint32_t {
    /// Start where told. If it holds, walk down while it keeps holding; if it
    /// does not, walk up until one does. **The fewest real-time runs that
    /// answer the question**, and the answer is the same one either way.
    adaptive,
    /// Only ever smaller. A start that underruns has no answer here, and that
    /// is reported rather than grown into one -- which is what somebody wants
    /// who is asking *how little can I get away with* rather than *what does
    /// this need*.
    shrink,
    /// Only ever larger, so the answer is never below where it started. For a
    /// machine already known to be short, and for not spending runs proving
    /// again that small rings are small.
    grow,
    /// Every size from the ceiling down, with no early stop. **Expensive on
    /// purpose**: the others assume that if a size held then every larger size
    /// holds, which is true of the mechanism and not always true of a machine
    /// with something else running on it.
    every,
};

[[nodiscard]] std::string_view sweep_name(Sweep sweep) noexcept;
[[nodiscard]] bool sweep_from_name(std::string_view name, Sweep& out) noexcept;

/// **Where in a file to measure, and for how long.**
///
/// Asked rather than assumed, for the reason at the top of this file: this is
/// the whole of what a calibration costs, in minutes and in sound out of
/// somebody's speakers.
struct Windows {
    /// How many places in each file to measure.
    std::uint32_t count = 3;
    /// How long each one plays.
    double seconds = 10.0;
};

/// The start of each window, in seconds, for a file `duration` long.
///
/// Spread evenly with the first at the start and the last ending at the end, so
/// a three-window plan measures the beginning, the middle and the run-out. A
/// file too short to hold the plan gets one window at the start and no
/// arithmetic pretending otherwise.
[[nodiscard]] std::vector<double> window_starts(const Windows& windows, double duration);

/// Everything a user chose before the first run was made.
struct CalibrationPlan {
    /// What to move. A dimension not named is not swept and not written down.
    Dimension dimensions = Dimension::ring;
    Sweep sweep = Sweep::adaptive;
    Windows windows;
    /// Where the sweep begins. The default is the ring's own default, so the
    /// first run made is the one the program would have made anyway.
    std::uint32_t start_ring = 128;
    /// How far it may walk in each direction before saying it found no answer.
    /// **Stopping points, not judgements about the value**: a user who names a
    /// ring outside these gets it, and this is only how far an automatic sweep
    /// goes before it admits it did not find one.
    std::uint32_t lowest_ring = 1;
    std::uint32_t highest_ring = 8192;
};

/// One thing to try: a file, a place in it, and the settings under test.
struct CalibrationRun {
    std::string file;
    double at_seconds = 0.0;
    double seconds = 10.0;
    /// The ring under test, in device periods.
    std::uint32_t ring_periods = 0;
    /// Threads for the video decoder, or 0 for whatever the build decides.
    std::uint32_t decoder_threads = 0;
};

/// What one run did. `ok` false means it could not be made at all, which is not
/// the same as a run that underran.
struct RunResult {
    bool ok = false;
    std::uint64_t underruns = 0;
    std::uint64_t frames_dropped = 0;
    /// Straight from `PassthroughGraph::Stats`, and the device's own numbers
    /// beside them, because milliseconds is what a profile stores.
    std::size_t low_water_bytes = 0;
    std::size_t ring_bytes = 0;
    std::uint32_t period_frames = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t frame_bytes = 0;
};

/// What a platform has to be able to do for a calibration to happen at all.
class ICalibrationHost {
public:
    virtual ~ICalibrationHost() = default;

    /// The class of stream in `file`, and how long it is. False and a reason
    /// when the file will not open or has no video in it -- which is a skipped
    /// file, not a failed calibration.
    virtual bool inspect(const std::string& file, StreamShape& shape, double& duration,
                         std::string& why) = 0;

    /// Plays one window with the settings in `run` and reports what happened.
    /// False and a reason when the run could not be made; a run that underran
    /// is a successful run with underruns in it.
    virtual bool play(const CalibrationRun& run, RunResult& result, std::string& why) = 0;

    /// One line of progress, because this takes minutes and a program that goes
    /// quiet for minutes is a program that has hung.
    virtual void say(const std::string& line) = 0;
};

/// What a calibration decided, and what it cost.
struct CalibrationReport {
    Profile profile;
    /// Files that could not be measured, each with why.
    std::vector<std::string> skipped;
    /// How many runs were made. What the afternoon went on.
    std::uint32_t runs = 0;
};

/// The sizes `sweep` will try, in order, before any of them has been run.
///
/// Powers of two, because `ByteRing` rounds capacity up to one and the sizes in
/// between are the same ring -- 8, 9 and 12 periods were measured as the same
/// run, to the frame. **`Sweep::adaptive` is not in here**: which way it goes is
/// decided by what the first run does, which is the point of it.
[[nodiscard]] std::vector<std::uint32_t> ring_order(const CalibrationPlan& plan);

/// Sweeps `files` and returns what it learnt.
[[nodiscard]] CalibrationReport calibrate(ICalibrationHost& host,
                                          const std::vector<std::string>& files,
                                          const CalibrationPlan& plan);

} // namespace mp

#endif

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/calibrate.hpp"

#include <algorithm>
#include <functional>
#include <thread>

namespace mp {
namespace {

struct NamedSweep {
    Sweep sweep;
    std::string_view name;
};

constexpr NamedSweep k_sweeps[] = {
    {Sweep::adaptive, "adaptive"},
    {Sweep::shrink, "shrink"},
    {Sweep::grow, "grow"},
    {Sweep::every, "every"},
};

/// A run is clean when the device never went short. Not "few underruns": one
/// underrun is one audible click, and the condition §9.8.2 measured against is
/// zero of them.
bool clean(const RunResult& result) noexcept
{
    return result.ok && result.underruns == 0;
}

double milliseconds(std::size_t bytes, std::uint32_t frame_bytes,
                    std::uint32_t sample_rate) noexcept
{
    if (frame_bytes == 0 || sample_rate == 0) {
        return 0.0;
    }
    return 1000.0 * static_cast<double>(bytes / frame_bytes) /
           static_cast<double>(sample_rate);
}

/// The power of two at or below `n`, so a start nobody rounded lands on a size
/// the ring can actually be.
std::uint32_t down_to_power_of_two(std::uint32_t n) noexcept
{
    std::uint32_t size = 1;
    while (size <= n / 2) {
        size *= 2;
    }
    return size;
}

/// Powers of two up to what the machine has, and never fewer than one. A
/// decoder given more threads than there are cores is a decoder given
/// contention.
std::vector<std::uint32_t> thread_candidates()
{
    const unsigned cores = std::thread::hardware_concurrency();
    const std::uint32_t most = cores == 0 ? 8u : static_cast<std::uint32_t>(cores);
    std::vector<std::uint32_t> out;
    for (std::uint32_t count = 1; count <= most; count *= 2) {
        out.push_back(count);
    }
    if (out.back() != most) {
        out.push_back(most);
    }
    return out;
}

/// What one ring size did across every window: whether all of them held, the
/// ring's size in milliseconds, and whether the run could be made at all.
struct Trial {
    bool made = true;
    bool held = false;
    double ring_ms = 0.0;
    /// The size this answer is, so a later dimension is measured at the ring
    /// the sweep settled on rather than at the one it started from.
    std::uint32_t periods = 0;
};

class Sweeper {
public:
    Sweeper(ICalibrationHost& host, const std::string& file,
            const std::vector<double>& starts, const CalibrationPlan& plan,
            CalibrationReport& report)
        : host_(host), file_(file), starts_(starts), plan_(plan), report_(report)
    {
    }

    /// **Every window has to hold, not their average.** One window is enough to
    /// disqualify a size, and the rest are not played: a real-time sweep that
    /// finishes a set it has already failed is spending minutes to learn
    /// nothing.
    Trial at(std::uint32_t periods)
    {
        Trial trial;
        CalibrationRun run;
        run.file = file_;
        run.seconds = plan_.windows.seconds;
        run.ring_periods = periods;

        for (const double when : starts_) {
            run.at_seconds = when;
            RunResult result;
            std::string why;
            if (!host_.play(run, result, why)) {
                report_.skipped.push_back(file_ + ": " + why);
                trial.made = false;
                return trial;
            }
            ++report_.runs;
            trial.ring_ms = std::max(trial.ring_ms,
                                     milliseconds(result.ring_bytes, result.frame_bytes,
                                                  result.sample_rate));
            if (!clean(result)) {
                host_.say(file_ + ": " + std::to_string(periods) + " periods underran");
                return trial;
            }
        }
        trial.held = true;
        host_.say(file_ + ": " + std::to_string(periods) + " periods held");
        return trial;
    }

private:
    ICalibrationHost& host_;
    const std::string& file_;
    const std::vector<double>& starts_;
    const CalibrationPlan& plan_;
    CalibrationReport& report_;
};

/// The answer for one file: the smallest size that held, and its ring in
/// milliseconds. `made` false means a run could not be made and the file is
/// skipped; `held` false means no size in range played it cleanly.
Trial sweep_ring(Sweeper& trials, const CalibrationPlan& plan)
{
    Trial answer;
    answer.made = true;
    answer.held = false;

    const std::uint32_t lowest = std::max(1u, plan.lowest_ring);
    const std::uint32_t highest = std::max(lowest, plan.highest_ring);
    const std::uint32_t start =
        std::clamp(down_to_power_of_two(std::max(1u, plan.start_ring)), lowest, highest);

    // Walks down from `from` while sizes keep holding, keeping the smallest one
    // that did.
    const auto shrink_from = [&](std::uint32_t from) {
        for (std::uint32_t size = from; size >= lowest; size /= 2) {
            const Trial trial = trials.at(size);
            if (!trial.made) {
                answer.made = false;
                return;
            }
            if (!trial.held) {
                return; // everything below is smaller and fails too
            }
            answer.held = true;
            answer.ring_ms = trial.ring_ms;
            answer.periods = size;
            if (size == 1 || size / 2 < lowest) {
                return;
            }
        }
    };

    // Walks up from `from` until one holds. **The first that holds is the
    // answer**: it is the smallest size at or above the start that worked.
    const auto grow_from = [&](std::uint32_t from) {
        for (std::uint64_t size = from; size <= highest; size *= 2) {
            const Trial trial = trials.at(static_cast<std::uint32_t>(size));
            if (!trial.made) {
                answer.made = false;
                return;
            }
            if (trial.held) {
                answer.held = true;
                answer.ring_ms = trial.ring_ms;
                answer.periods = static_cast<std::uint32_t>(size);
                return;
            }
        }
    };

    switch (plan.sweep) {
    case Sweep::shrink:
        shrink_from(start);
        break;
    case Sweep::grow:
        grow_from(start);
        break;
    case Sweep::every: {
        // No early stop, and the smallest that held wins. The others assume a
        // size holding means every larger size holds; this one does not.
        for (std::uint32_t size = highest; size >= lowest; size /= 2) {
            const Trial trial = trials.at(size);
            if (!trial.made) {
                answer.made = false;
                return answer;
            }
            if (trial.held) {
                answer.held = true;
                answer.ring_ms = trial.ring_ms;
                answer.periods = size;
            }
            if (size == 1 || size / 2 < lowest) {
                break;
            }
        }
        break;
    }
    case Sweep::adaptive: {
        // **The start decides the direction.** It held, so smaller may hold
        // too; it did not, so nothing smaller will and the question is how much
        // more is needed.
        const Trial first = trials.at(start);
        if (!first.made) {
            answer.made = false;
            return answer;
        }
        if (first.held) {
            answer.held = true;
            answer.ring_ms = first.ring_ms;
            answer.periods = start;
            if (start > 1 && start / 2 >= lowest) {
                shrink_from(start / 2);
            }
        } else if (std::uint64_t{start} * 2 <= highest) {
            grow_from(static_cast<std::uint32_t>(std::uint64_t{start} * 2));
        }
        break;
    }
    }
    return answer;
}

/// The thread count that dropped the fewest frames, at the ring the sweep
/// settled on.
///
/// **Underruns decide first even here.** §8 makes the audio the clock and the
/// picture what gives way, so a thread count that keeps more frames by starving
/// the ring has not done better, it has done the one thing it may not. Ties go
/// to the smaller count: cores this does not take are cores something else can.
std::uint32_t best_threads(ICalibrationHost& host, const std::string& file,
                           const std::vector<double>& starts, const CalibrationPlan& plan,
                           std::uint32_t ring, CalibrationReport& report)
{
    std::uint32_t best = 0;
    std::uint64_t best_underruns = 0;
    std::uint64_t best_dropped = 0;

    for (const std::uint32_t count : thread_candidates()) {
        CalibrationRun run;
        run.file = file;
        run.seconds = plan.windows.seconds;
        run.ring_periods = ring;
        run.decoder_threads = count;

        bool made = true;
        std::uint64_t underruns = 0;
        std::uint64_t dropped = 0;
        for (const double when : starts) {
            run.at_seconds = when;
            RunResult result;
            std::string why;
            if (!host.play(run, result, why)) {
                report.skipped.push_back(file + ": " + why);
                made = false;
                break;
            }
            ++report.runs;
            underruns += result.underruns;
            dropped += result.frames_dropped;
        }
        if (!made) {
            continue;
        }
        host.say(file + ": " + std::to_string(count) + " threads dropped " +
                 std::to_string(dropped) + " and underran " + std::to_string(underruns));
        const bool better = best == 0 || underruns < best_underruns ||
                            (underruns == best_underruns && dropped < best_dropped);
        if (better) {
            best = count;
            best_underruns = underruns;
            best_dropped = dropped;
        }
    }
    return best;
}

} // namespace

std::string_view sweep_name(Sweep sweep) noexcept
{
    for (const NamedSweep& named : k_sweeps) {
        if (named.sweep == sweep) {
            return named.name;
        }
    }
    return {};
}

bool sweep_from_name(std::string_view name, Sweep& out) noexcept
{
    for (const NamedSweep& named : k_sweeps) {
        if (named.name == name) {
            out = named.sweep;
            return true;
        }
    }
    return false;
}

std::vector<double> window_starts(const Windows& windows, double duration)
{
    std::vector<double> out;
    const double length = windows.seconds > 0.0 ? windows.seconds : 0.0;
    if (windows.count == 0 || length <= 0.0) {
        return out;
    }
    // A file that cannot hold the plan gets one window and no arithmetic
    // pretending otherwise: measuring the same ten seconds three times would
    // report three runs and one fact.
    if (!(duration > length) || windows.count == 1) {
        out.push_back(0.0);
        return out;
    }
    // The last window ends where the file does and the first starts where it
    // starts: the beginning, the middle and the run-out, which is where a
    // decoder's work is least like the average.
    const double last = duration - length;
    const double step = last / static_cast<double>(windows.count - 1);
    for (std::uint32_t i = 0; i < windows.count; ++i) {
        out.push_back(step * static_cast<double>(i));
    }
    return out;
}

std::vector<std::uint32_t> ring_order(const CalibrationPlan& plan)
{
    std::vector<std::uint32_t> out;
    const std::uint32_t lowest = std::max(1u, plan.lowest_ring);
    const std::uint32_t highest = std::max(lowest, plan.highest_ring);
    const std::uint32_t start =
        std::clamp(down_to_power_of_two(std::max(1u, plan.start_ring)), lowest, highest);

    switch (plan.sweep) {
    case Sweep::shrink:
        for (std::uint32_t size = start; size >= lowest; size /= 2) {
            out.push_back(size);
            if (size == 1 || size / 2 < lowest) {
                break;
            }
        }
        break;
    case Sweep::grow:
        for (std::uint64_t size = start; size <= highest; size *= 2) {
            out.push_back(static_cast<std::uint32_t>(size));
        }
        break;
    case Sweep::every:
        for (std::uint32_t size = highest; size >= lowest; size /= 2) {
            out.push_back(size);
            if (size == 1 || size / 2 < lowest) {
                break;
            }
        }
        break;
    case Sweep::adaptive:
        // Only the first is known before it has been run. Saying so with one
        // entry beats inventing an order that the second run may contradict.
        out.push_back(start);
        break;
    }
    return out;
}

CalibrationReport calibrate(ICalibrationHost& host, const std::vector<std::string>& files,
                            const CalibrationPlan& plan)
{
    CalibrationReport report;
    if (plan.dimensions == Dimension::none) {
        return report;
    }

    for (const std::string& file : files) {
        StreamShape shape;
        double duration = 0.0;
        std::string why;
        if (!host.inspect(file, shape, duration, why)) {
            report.skipped.push_back(file + ": " + why);
            continue;
        }
        const std::vector<double> starts = window_starts(plan.windows, duration);
        if (starts.empty()) {
            report.skipped.push_back(file + ": there is no window to measure in");
            continue;
        }

        Measurement measured;
        measured.shape = shape;
        measured.file = file;
        const std::uint32_t runs_before = report.runs;
        std::uint32_t settled = plan.start_ring;

        // **The ring first, and the thread count measured at the ring it
        // settled on.** Sweeping both together is a grid, and a grid of
        // real-time runs is an afternoon rather than a few minutes.
        if (has(plan.dimensions, Dimension::ring)) {
            Sweeper trials{host, file, starts, plan, report};
            const Trial answer = sweep_ring(trials, plan);
            if (!answer.made) {
                continue; // the reason is already in `skipped`
            }
            if (!answer.held) {
                report.skipped.push_back(
                    file + ": no ring this sweep tried played it without an underrun");
                continue;
            }
            // **One doubling of margin.** The smallest size that held is the
            // edge of the cliff: 16 periods was the least ring that held over
            // the 4K measurement and the acceptance ran at 32. A default does
            // not sit on an edge.
            measured.ring_ms = answer.ring_ms * 2.0;
            settled = answer.periods;
        }

        if (has(plan.dimensions, Dimension::decoder_threads)) {
            measured.decoder_threads =
                best_threads(host, file, starts, plan,
                             settled != 0 ? settled : plan.start_ring, report);
        }

        measured.runs = report.runs - runs_before;
        report.profile.measured.push_back(std::move(measured));
    }
    return report;
}

} // namespace mp

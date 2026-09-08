// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/clock.hpp"

namespace mp {

FreeClock::FreeClock(const ITicks& counter, std::uint32_t rate) noexcept
    : counter_(&counter), rate_(rate)
{
    spec_.wire_rate = rate;
    spec_.source_rate = rate;
    // Filled by the loop from its frame clock, which is the same counter this
    // stamps its readings with.
    spec_.tick_rate = 0;
}

std::uint64_t FreeClock::counted() const noexcept
{
    if (!started_) {
        return 0;
    }
    const std::uint64_t now = paused_ ? pause_began_ : counter_->now();
    const std::uint64_t tick_rate = counter_->rate();
    const std::uint64_t spent = began_ + held_;
    if (tick_rate == 0 || now <= spent) {
        return 0;
    }
    // One multiply before the divide, so a tick is not rounded away: an hour
    // at ten megahertz times forty-eight thousand is 1.7e17, inside sixty-four
    // bits.
    return (now - spent) * rate_ / tick_rate;
}

ClockSpec FreeClock::spec() const
{
    const std::lock_guard lock{lock_};
    return spec_;
}

bool FreeClock::read(ClockReading& out)
{
    const std::lock_guard lock{lock_};
    if (!started_) {
        return false;
    }
    out.device_frames = counted();
    // **Stamped now, even while paused.** A follower extrapolates from the
    // reading by the time since it was taken; a frozen count stamped with a
    // stale tick would be read as a count that has moved on.
    out.ticks = counter_->now();
    return true;
}

void FreeClock::start()
{
    const std::lock_guard lock{lock_};
    began_ = counter_->now();
    held_ = 0;
    paused_ = false;
    started_ = true;
}

void FreeClock::pause()
{
    const std::lock_guard lock{lock_};
    if (!started_ || paused_) {
        return;
    }
    pause_began_ = counter_->now();
    paused_ = true;
}

void FreeClock::resume()
{
    const std::lock_guard lock{lock_};
    if (!paused_) {
        return;
    }
    const std::uint64_t now = counter_->now();
    held_ += now > pause_began_ ? now - pause_began_ : 0;
    paused_ = false;
}

bool FreeClock::paused() const
{
    const std::lock_guard lock{lock_};
    return paused_;
}

void FreeClock::seek(std::uint64_t frame)
{
    const std::lock_guard lock{lock_};
    // The anchor moves and the count does not, exactly as a device's does: see
    // `ClockSpec::origin_device_frame`.
    spec_.origin_device_frame = counted();
    spec_.origin_source_frame = frame;
}

std::uint64_t FreeClock::position() const
{
    const std::lock_guard lock{lock_};
    const std::uint64_t count = counted();
    return spec_.origin_source_frame +
           (count > spec_.origin_device_frame ? count - spec_.origin_device_frame : 0);
}

} // namespace mp

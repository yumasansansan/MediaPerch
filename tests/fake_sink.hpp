// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A sink module implemented in the test, behind the real C vtable.
//
// It is the `sink_capture` of the plan arriving early, and it earns its keep
// twice: the negotiation rules can be tested against a device that refuses
// exactly what a real driver refuses, and the passthrough graph can be checked
// byte for byte with no hardware anywhere. The ABI is also exercised from the
// implementing side, which is the side a third-party module is on.

#include "mediaperch/format.hpp"
#include "mediaperch/sink.hpp"

#include "test_platform.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>


namespace mp::test {
class FakeSink;
} // namespace mp::test

/// The module ABI's handle for a sink, completed for the test program by the
/// one sink it implements: a member of each FakeSink that says which one it
/// is, so that the handle is an object of its own type and the thunks find
/// their sink through it rather than by taking the handle for the sink.
struct MpSink {
    mp::test::FakeSink* owner = nullptr;
};

namespace mp::test {

/// The device's clock: test_platform.hpp says why it is a timer of the
/// system's on Windows, and the standard library's sleep elsewhere.
using platform::PeriodTimer;

/// Behaviour a test wants out of the fake device.
struct FakeSinkRules {
    /// Returns true if this exact format should be accepted.
    std::function<bool(const Format&)> accepts = [](const Format&) { return true; };
    /// Frames handed out per acquire.
    std::uint32_t period_frames = 64;
    /// Answer `wait` with this many times before returning MP_OK for ever.
    std::uint32_t timeouts_before_ok = 0;
    /// Microseconds of the device's clock per period: `wait` says the device is
    /// ready when the period is due, a period every `pace_us` from when the
    /// device started taking them.
    ///
    /// Zero -- the default -- is a device with no clock at all, which is what
    /// most tests want: they check what bytes came out, not when. A test about
    /// *transport* cannot use that, because gapless and seek are claims about a
    /// stream that keeps up, and a render thread with nothing to wait for
    /// outruns the decode thread by a factor of thousands.
    ///
    /// **To a deadline, and by a clock that can keep one.** `wait` slept for
    /// `pace_us` once a period, and on Windows a sleep of 500 us is a whole
    /// timer tick, 15.5 ms: the device played 64 frames of 44.1 kHz in 15.5 ms,
    /// a tenth of real time, where it was meant to play them three times faster
    /// than that, and a quarter-second track took four seconds -- as long as a
    /// test waits for a track to end. Each period is now slept to its own
    /// deadline, so a wait that wakes late is made up by the ones after it,
    /// and slept there by `PeriodTimer`, so that the periods come one at a
    /// time, as a device's do, and not a tick's worth at once.
    std::uint32_t pace_us = 0;
    /// Hand back a format that is not the one asked for. Models the driver that
    /// says yes and means something else.
    std::function<Format(const Format&)> distort = nullptr;
    /// Take the device away after this many successful `wait` calls. Zero --
    /// the default -- is a device that stays.
    ///
    /// Models the one failure a player cannot argue with: somebody pulled the
    /// USB cable out. Everything above the sink is still perfectly alive, which
    /// is exactly why this is worth modelling rather than treating as the end.
    std::uint32_t waits_before_loss = 0;
    /// What losing it looks like. `MP_ERR_DEVICE_LOST` is what the WASAPI sink
    /// maps `AUDCLNT_E_DEVICE_INVALIDATED` and `AUDCLNT_E_RESOURCES_INVALIDATED`
    /// to, and it is the only error a host is expected to recover from.
    MpResult loss_result = MP_ERR_DEVICE_LOST;
    /// Whether this device has §8's clock at all.
    ///
    /// Off by default, and the default is the interesting case as much as the
    /// other one: a sink module that does not implement `get_position` leaves
    /// the vtable entry null, and a host that assumed a clock would read one
    /// through a null pointer. So the fake leaves it null too, and a test that
    /// wants a clock says so and then drives it with `set_clock`.
    bool has_clock = false;
};

class FakeSink {
public:
    explicit FakeSink(FakeSinkRules rules) : rules_(std::move(rules))
    {
        vtbl_.size = sizeof(MpSinkVtbl);
        vtbl_.negotiate = &FakeSink::negotiate_thunk;
        vtbl_.get_period = &FakeSink::get_period_thunk;
        vtbl_.start = &FakeSink::start_thunk;
        vtbl_.stop = &FakeSink::stop_thunk;
        vtbl_.close = &FakeSink::close_thunk;
        vtbl_.wait = &FakeSink::wait_thunk;
        vtbl_.acquire = &FakeSink::acquire_thunk;
        vtbl_.commit = &FakeSink::commit_thunk;
        if (rules_.has_clock) {
            vtbl_.get_position = &FakeSink::get_position_thunk;
        }
    }

    /// What the device says it has played, and the tick it says so at. A test
    /// drives this by hand: a clock nobody controls is one no assertion can be
    /// made against, and every question §8 asks -- what happens when it runs
    /// fast, when it runs slow, when it stops -- is a question about a number
    /// somebody chose.
    void set_clock(std::uint64_t device_frames, std::uint64_t ticks) noexcept
    {
        const std::lock_guard lock{mutex_};
        clock_frames_ = device_frames;
        clock_ticks_ = ticks;
        told_ = true;
    }

    /// A `mp::Sink` pointing at this object. Non-owning: `close` is a no-op, so
    /// the Sink destructor cannot take the test's object with it.
    [[nodiscard]] Sink handle() noexcept
    {
        return Sink{&vtbl_, &as_handle_};
    }

    [[nodiscard]] const std::vector<Format>& offered() const noexcept { return offered_; }
    [[nodiscard]] const Format& accepted() const noexcept { return accepted_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

    /// Everything that was committed, in order. The bit-exactness check.
    [[nodiscard]] std::vector<std::uint8_t> captured() const
    {
        const std::lock_guard lock{mutex_};
        return captured_;
    }

    [[nodiscard]] std::size_t captured_size() const
    {
        const std::lock_guard lock{mutex_};
        return captured_.size();
    }

private:
    static FakeSink& self(MpSink* s) noexcept { return *s->owner; }

    static MpResult MP_CALL get_position_thunk(MpSink* s, std::uint64_t* frames,
                                               std::uint64_t* ticks)
    {
        if (frames == nullptr || ticks == nullptr) {
            return MP_ERR_INVALID;
        }
        FakeSink& me = self(s);
        const std::lock_guard lock{me.mutex_};
        if (!me.told_) {
            // **A device nobody has driven has no position, and saying zero
            // would be worse than saying nothing.** A zero frame count stamped
            // with tick zero reads as a device that started at the epoch, so
            // §8's extrapolation carries it forward by however long this
            // machine has been up -- which drops every video frame there will
            // ever be. `AvClock` is right to refuse a reading it has not had;
            // this is the fake learning to withhold one.
            return MP_ERR_UNSUPPORTED;
        }
        *frames = me.clock_frames_;
        *ticks = me.clock_ticks_;
        return MP_OK;
    }

    static MpResult MP_CALL negotiate_thunk(MpSink* s, const MpFormat* want, MpFormat* out)
    {
        FakeSink& me = self(s);
        const Format asked = from_abi(*want);
        me.offered_.push_back(asked);
        if (!me.rules_.accepts(asked)) {
            return MP_ERR_FORMAT;
        }
        me.accepted_ = me.rules_.distort ? me.rules_.distort(asked) : asked;
        me.frame_bytes_ = frame_bytes(me.accepted_);
        *out = to_abi(me.accepted_);
        return MP_OK;
    }

    static MpResult MP_CALL get_period_thunk(MpSink* s, std::uint32_t* frames)
    {
        *frames = self(s).rules_.period_frames;
        return MP_OK;
    }

    static MpResult MP_CALL start_thunk(MpSink* s)
    {
        self(s).started_ = true;
        self(s).paced_.store(false, std::memory_order_relaxed);
        return MP_OK;
    }

    static MpResult MP_CALL stop_thunk(MpSink* s)
    {
        self(s).started_ = false;
        self(s).paced_.store(false, std::memory_order_relaxed);
        return MP_OK;
    }

    static void MP_CALL close_thunk(MpSink*) {}

    static MpResult MP_CALL wait_thunk(MpSink* s, std::uint32_t)
    {
        FakeSink& me = self(s);
        if (me.waits_++ < me.rules_.timeouts_before_ok) {
            return MP_TIMEOUT;
        }
        if (me.rules_.waits_before_loss != 0 && me.waits_ > me.rules_.waits_before_loss) {
            return me.rules_.loss_result;
        }
        if (me.rules_.pace_us != 0) {
            // A device behind by more than a buffer's worth of periods -- the
            // first wait, a start, a render thread that stopped asking for a
            // while -- starts afresh from now rather than playing the gap back
            // at once, as a device that ran dry does. The allowance is 32 ms at
            // 500 us a period, so that a render thread the system did not run
            // for a few milliseconds is caught up, not forgotten.
            const auto period = std::chrono::microseconds{me.rules_.pace_us};
            const auto now = std::chrono::steady_clock::now();
            if (!me.paced_.exchange(true, std::memory_order_relaxed) ||
                now - me.due_ > period * k_backlog_periods) {
                me.due_ = now;
            }
            me.due_ += period;
            me.timer_.sleep_until(me.due_);
        }
        return MP_OK;
    }

    static MpResult MP_CALL acquire_thunk(MpSink* s, void** ptr, std::uint32_t* frames)
    {
        FakeSink& me = self(s);
        me.scratch_.assign(static_cast<std::size_t>(me.rules_.period_frames) * me.frame_bytes_,
                           0xCD); // a pattern, so an uncommitted gap is visible
        *ptr = me.scratch_.data();
        *frames = me.rules_.period_frames;
        return MP_OK;
    }

    static MpResult MP_CALL commit_thunk(MpSink* s, std::uint32_t frames, std::uint32_t)
    {
        FakeSink& me = self(s);
        const std::size_t bytes = static_cast<std::size_t>(frames) * me.frame_bytes_;
        const std::lock_guard lock{me.mutex_};
        me.captured_.insert(me.captured_.end(), me.scratch_.begin(),
                            me.scratch_.begin() + static_cast<std::ptrdiff_t>(bytes));
        return MP_OK;
    }

    MpSinkVtbl vtbl_{};
    /// The handle the vtable is handed back, which says whose it is.
    MpSink as_handle_{this};
    std::uint64_t clock_frames_ = 0;
    std::uint64_t clock_ticks_ = 0;
    /// Whether anybody has said where the device is. Until somebody has, it
    /// has no position -- see `get_position_thunk`.
    bool told_ = false;
    FakeSinkRules rules_;
    std::vector<Format> offered_;
    Format accepted_{};
    std::uint32_t frame_bytes_ = 0;
    bool started_ = false;
    std::uint32_t waits_ = 0;
    /// When the next period is due (see `FakeSinkRules::pace_us`), whether
    /// that has been set since the device last started, and what sleeps to it.
    /// The deadline and the timer are the render thread's alone; the flag is
    /// cleared by `start` and `stop`, which the engine's thread calls too.
    static constexpr int k_backlog_periods = 64;
    std::chrono::steady_clock::time_point due_{};
    std::atomic<bool> paced_{false};
    PeriodTimer timer_;

    std::vector<std::uint8_t> scratch_;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> captured_;
};

} // namespace mp::test

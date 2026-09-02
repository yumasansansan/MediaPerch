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

#include <cstdint>
#include <cstring>
#include <functional>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace mp::test {

/// Behaviour a test wants out of the fake device.
struct FakeSinkRules {
    /// Returns true if this exact format should be accepted.
    std::function<bool(const Format&)> accepts = [](const Format&) { return true; };
    /// Frames handed out per acquire.
    std::uint32_t period_frames = 64;
    /// Answer `wait` with this many times before returning MP_OK for ever.
    std::uint32_t timeouts_before_ok = 0;
    /// Microseconds `wait` sleeps before saying the device is ready.
    ///
    /// Zero -- the default -- is a device with no clock at all, which is what
    /// most tests want: they check what bytes came out, not when. A test about
    /// *transport* cannot use that, because gapless and seek are claims about a
    /// stream that keeps up, and a render thread with nothing to wait for
    /// outruns the decode thread by a factor of thousands.
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
    }

    /// A `mp::Sink` pointing at this object. Non-owning: `close` is a no-op, so
    /// the Sink destructor cannot take the test's object with it.
    [[nodiscard]] Sink handle() noexcept
    {
        return Sink{&vtbl_, reinterpret_cast<MpSink*>(this)};
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
    static FakeSink& self(MpSink* s) noexcept { return *reinterpret_cast<FakeSink*>(s); }

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
        return MP_OK;
    }

    static MpResult MP_CALL stop_thunk(MpSink* s)
    {
        self(s).started_ = false;
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
            std::this_thread::sleep_for(std::chrono::microseconds{me.rules_.pace_us});
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
    FakeSinkRules rules_;
    std::vector<Format> offered_;
    Format accepted_{};
    std::uint32_t frame_bytes_ = 0;
    bool started_ = false;
    std::uint32_t waits_ = 0;

    std::vector<std::uint8_t> scratch_;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> captured_;
};

} // namespace mp::test

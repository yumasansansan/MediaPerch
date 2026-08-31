// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

namespace mp {

/// The handover between the decode thread and the render thread, and the only
/// state the two share.
///
/// Single producer, single consumer, power-of-two capacity, free-running
/// indices, acquire/release, no CAS and no lock. `read` is called from the
/// MMCSS render thread and therefore allocates nothing, waits on nothing, and
/// cannot fail -- a short read is an underrun, which is data, not an exception.
///
/// It holds *bytes*, not samples. In the passthrough graph those bytes are
/// already in the device's format and nothing in this class knows or cares what
/// they mean.
class ByteRing {
public:
    /// Rounds `capacity_bytes` up to a power of two, minimum 64.
    explicit ByteRing(std::size_t capacity_bytes)
        : buf_(round_up_pow2(capacity_bytes)), mask_(buf_.size() - 1)
    {
    }

    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;
    ByteRing(ByteRing&&) = delete;
    ByteRing& operator=(ByteRing&&) = delete;
    ~ByteRing() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

    /// Producer side.
    [[nodiscard]] std::size_t writable() const noexcept
    {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return buf_.size() - (w - r);
    }

    /// Consumer side. Safe to call from the render thread.
    [[nodiscard]] std::size_t readable() const noexcept
    {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return w - r;
    }

    /// Copies as much of `src` as fits. Returns how much went in, which is less
    /// than `bytes` when the ring is full.
    std::size_t write(const void* src, std::size_t bytes) noexcept
    {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t room = buf_.size() - (w - r);
        const std::size_t n = bytes < room ? bytes : room;
        if (n == 0) {
            return 0;
        }

        const std::size_t offset = w & mask_;
        const std::size_t first = std::min(n, buf_.size() - offset);
        const auto* in = static_cast<const std::byte*>(src);
        std::memcpy(buf_.data() + offset, in, first);
        if (n > first) {
            std::memcpy(buf_.data(), in + first, n - first);
        }

        write_.store(w + n, std::memory_order_release);
        return n;
    }

    /// Copies out at most `bytes`. Returns how much came out; a short read is an
    /// underrun and the caller decides what to do about it (the render thread
    /// fills the remainder with silence and counts it).
    ///
    /// MP_RT: allocates nothing, blocks on nothing.
    std::size_t read(void* dst, std::size_t bytes) noexcept
    {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t have = w - r;
        const std::size_t n = bytes < have ? bytes : have;
        if (n == 0) {
            return 0;
        }

        const std::size_t offset = r & mask_;
        const std::size_t first = std::min(n, buf_.size() - offset);
        auto* out = static_cast<std::byte*>(dst);
        std::memcpy(out, buf_.data() + offset, first);
        if (n > first) {
            std::memcpy(out + first, buf_.data(), n - first);
        }

        read_.store(r + n, std::memory_order_release);
        return n;
    }

    /// Drops everything. Legal only when both sides are stopped -- that is, at a
    /// graph rebuild point, which is the only time the graph changes anyway.
    void reset() noexcept
    {
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

private:
    static std::size_t round_up_pow2(std::size_t n) noexcept
    {
        if (n < 64) {
            return 64;
        }
        return std::has_single_bit(n) ? n : std::bit_ceil(n);
    }

    static constexpr std::size_t cache_line = 64;

    std::vector<std::byte> buf_;
    std::size_t mask_;

    // The two indices are written by different threads. Sharing a cache line
    // between them turns every handover into a coherence miss on a thread that
    // has a 3 ms deadline.
    alignas(cache_line) std::atomic<std::size_t> write_{0};
    alignas(cache_line) std::atomic<std::size_t> read_{0};
};

} // namespace mp

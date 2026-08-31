// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

TEST_CASE("capacity rounds up to a power of two", "[ring]")
{
    CHECK(mp::ByteRing{1}.capacity() == 64);
    CHECK(mp::ByteRing{64}.capacity() == 64);
    CHECK(mp::ByteRing{65}.capacity() == 128);
    CHECK(mp::ByteRing{4096}.capacity() == 4096);
    CHECK(mp::ByteRing{5000}.capacity() == 8192);
}

TEST_CASE("bytes come out in the order they went in", "[ring]")
{
    mp::ByteRing ring{64};
    std::array<std::uint8_t, 32> in{};
    std::iota(in.begin(), in.end(), std::uint8_t{0});

    REQUIRE(ring.write(in.data(), in.size()) == in.size());
    CHECK(ring.readable() == 32);
    CHECK(ring.writable() == 32);

    std::array<std::uint8_t, 32> out{};
    REQUIRE(ring.read(out.data(), out.size()) == out.size());
    CHECK(out == in);
    CHECK(ring.readable() == 0);
}

TEST_CASE("a full ring accepts a short write rather than overwriting", "[ring]")
{
    mp::ByteRing ring{64};
    const std::vector<std::uint8_t> in(100, 0xAB);

    CHECK(ring.write(in.data(), in.size()) == 64);
    CHECK(ring.write(in.data(), in.size()) == 0);
    CHECK(ring.writable() == 0);
}

TEST_CASE("a short read is an underrun, not an error", "[ring]")
{
    mp::ByteRing ring{64};
    const std::array<std::uint8_t, 8> in{1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(ring.write(in.data(), in.size()) == 8);

    std::array<std::uint8_t, 32> out{};
    out.fill(0xFF);
    CHECK(ring.read(out.data(), out.size()) == 8);

    // The render thread fills the remainder with silence and counts it; the ring
    // does not touch what it did not write.
    CHECK(out[7] == 8);
    CHECK(out[8] == 0xFF);
}

TEST_CASE("writes and reads wrap the buffer without reordering", "[ring]")
{
    mp::ByteRing ring{64};
    std::uint8_t next_written = 0;
    std::uint8_t next_expected = 0;

    // Uneven chunk sizes so that every read and every write straddles the end of
    // the buffer sooner or later.
    for (int round = 0; round < 200; ++round) {
        std::array<std::uint8_t, 13> chunk{};
        for (auto& b : chunk) {
            b = next_written++;
        }
        REQUIRE(ring.write(chunk.data(), chunk.size()) == chunk.size());

        std::array<std::uint8_t, 13> out{};
        REQUIRE(ring.read(out.data(), out.size()) == out.size());
        for (const auto b : out) {
            REQUIRE(b == next_expected++);
        }
    }
}

TEST_CASE("reset drops everything", "[ring]")
{
    mp::ByteRing ring{64};
    const std::array<std::uint8_t, 16> in{};
    REQUIRE(ring.write(in.data(), in.size()) == 16);

    ring.reset();
    CHECK(ring.readable() == 0);
    CHECK(ring.writable() == 64);
}

TEST_CASE("one producer and one consumer agree on every byte", "[ring][slow]")
{
    // The real usage: a decode thread ahead of a render thread, with neither
    // taking a lock. A reordering bug here shows up as a click once an hour on
    // somebody else's machine, so it gets a long run rather than a spot check.
    constexpr std::size_t total = 1u << 20;
    mp::ByteRing ring{4096};
    std::atomic<bool> producer_done{false};

    std::thread producer{[&] {
        std::size_t written = 0;
        std::uint8_t value = 0;
        std::array<std::uint8_t, 97> chunk{};
        while (written < total) {
            const std::size_t want = std::min(chunk.size(), total - written);
            for (std::size_t i = 0; i < want; ++i) {
                chunk[i] = static_cast<std::uint8_t>(value + i);
            }
            const std::size_t n = ring.write(chunk.data(), want);
            value = static_cast<std::uint8_t>(value + n);
            written += n;
            if (n == 0) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    }};

    std::size_t read_total = 0;
    std::uint8_t expected = 0;
    bool ordered = true;
    std::array<std::uint8_t, 61> out{};
    while (read_total < total) {
        const std::size_t n = ring.read(out.data(), out.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] != expected) {
                ordered = false;
            }
            ++expected;
        }
        read_total += n;
        if (n == 0 && !producer_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    producer.join();
    CHECK(ordered);
    CHECK(read_total == total);
}

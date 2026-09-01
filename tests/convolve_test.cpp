// SPDX-License-Identifier: GPL-3.0-or-later
//
// The convolver, against the definition of convolution.
//
// There is nothing to interpret here: a convolution has an answer, the direct
// form computes it, and the partitioned frequency-domain form has to produce
// the same numbers to within what double arithmetic can be asked for. So the
// test computes both and subtracts them.

#include <convolve.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

/// The definition, written out.
std::vector<double> direct(const std::vector<double>& x, const std::vector<double>& h)
{
    std::vector<double> y(x.size() + h.size() - 1, 0.0);
    for (std::size_t n = 0; n < x.size(); ++n) {
        for (std::size_t k = 0; k < h.size(); ++k) {
            y[n + k] += x[n] * h[k];
        }
    }
    return y;
}

/// A fixed generator: a convolution that is right on one draw and wrong on
/// another is not right.
std::vector<double> noise(std::size_t n, std::uint32_t seed)
{
    std::vector<double> out(n);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    }
    return out;
}

/// Runs a signal through in blocks and drains it, the way a graph would.
std::vector<double> through(mp::convolve::Convolver& convolver,
                            const std::vector<double>& x, std::uint32_t block)
{
    std::vector<double> out;
    std::vector<double> scratch(convolver.max_output(block) + 8);
    double* planes[] = {scratch.data()};

    for (std::size_t at = 0; at < x.size(); at += block) {
        const auto n =
            static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - at));
        const double* in[] = {x.data() + at};
        std::uint32_t produced = 0;
        convolver.process(in, n, planes, static_cast<std::uint32_t>(scratch.size()),
                          produced);
        out.insert(out.end(), scratch.begin(),
                   scratch.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    for (int round = 0; round < 4096; ++round) {
        std::uint32_t produced = 0;
        convolver.flush(planes, static_cast<std::uint32_t>(scratch.size()), produced);
        if (produced == 0) {
            break;
        }
        out.insert(out.end(), scratch.begin(),
                   scratch.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    return out;
}

} // namespace

TEST_CASE("the partitioned convolution is the convolution", "[convolve]")
{
    // Several impulse lengths against several partition sizes, because the
    // interesting bugs live where the last partition is not full and where the
    // block a caller uses and the partition have nothing in common.
    for (const std::size_t taps : {std::size_t{1}, std::size_t{7}, std::size_t{64},
                                   std::size_t{100}, std::size_t{512}, std::size_t{1000}}) {
        for (const std::uint32_t partition : {16u, 64u, 256u}) {
            const auto h = noise(taps, 12345u + static_cast<std::uint32_t>(taps));
            const auto x = noise(3000, 999u);

            mp::convolve::Convolver convolver;
            std::string why;
            INFO("taps " << taps << ", partition " << partition);
            REQUIRE(convolver.configure(h, 1, partition, why));

            const auto want = direct(x, h);
            const auto got = through(convolver, x, 333);
            REQUIRE(got.size() == want.size());
            for (std::size_t i = 0; i < want.size(); ++i) {
                INFO("frame " << i);
                REQUIRE(got[i] == Catch::Approx(want[i]).margin(1e-12));
            }
        }
    }
}

TEST_CASE("a unit impulse is a wire, and a delayed one is a delay", "[convolve]")
{
    const auto x = noise(2000, 7u);
    std::string why;

    mp::convolve::Convolver wire;
    REQUIRE(wire.configure({1.0}, 1, 64, why));
    const auto same = through(wire, x, 128);
    REQUIRE(same.size() == x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        REQUIRE(same[i] == Catch::Approx(x[i]).margin(1e-14));
    }

    // Output frame n is the convolution at frame n: the engine shifts nothing,
    // and whatever shifts is what the impulse says.
    std::vector<double> late(17, 0.0);
    late.back() = 1.0;
    mp::convolve::Convolver delay;
    REQUIRE(delay.configure(late, 1, 64, why));
    const auto shifted = through(delay, x, 100);
    REQUIRE(shifted.size() == x.size() + 16);
    for (std::size_t i = 0; i < 16; ++i) {
        REQUIRE(shifted[i] == Catch::Approx(0.0).margin(1e-14));
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        REQUIRE(shifted[i + 16] == Catch::Approx(x[i]).margin(1e-14));
    }
}

TEST_CASE("the block size the graph happens to use changes nothing", "[convolve]")
{
    const auto h = noise(700, 4u);
    const auto x = noise(5000, 5u);
    std::string why;

    mp::convolve::Convolver reference;
    REQUIRE(reference.configure(h, 1, 128, why));
    const auto want = through(reference, x, 5000);

    for (const std::uint32_t block : {1u, 13u, 128u, 577u, 4096u}) {
        mp::convolve::Convolver convolver;
        REQUIRE(convolver.configure(h, 1, 128, why));
        const auto got = through(convolver, x, block);
        INFO("block " << block);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            REQUIRE(got[i] == Catch::Approx(want[i]).margin(1e-12));
        }
    }
}

TEST_CASE("channels do not leak into each other", "[convolve]")
{
    const auto h = noise(300, 3u);
    const auto left = noise(2000, 1u);
    const auto right = noise(2000, 2u);

    mp::convolve::Convolver convolver;
    std::string why;
    REQUIRE(convolver.configure(h, 2, 64, why));

    std::vector<double> a(convolver.max_output(256) + 8);
    std::vector<double> b(a.size());
    std::vector<double> out_left;
    std::vector<double> out_right;
    double* planes[] = {a.data(), b.data()};

    for (std::size_t at = 0; at < left.size(); at += 256) {
        const auto n =
            static_cast<std::uint32_t>(std::min<std::size_t>(256, left.size() - at));
        const double* in[] = {left.data() + at, right.data() + at};
        std::uint32_t produced = 0;
        convolver.process(in, n, planes, static_cast<std::uint32_t>(a.size()), produced);
        out_left.insert(out_left.end(), a.begin(),
                        a.begin() + static_cast<std::ptrdiff_t>(produced));
        out_right.insert(out_right.end(), b.begin(),
                         b.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    const auto want_left = direct(left, h);
    const auto want_right = direct(right, h);
    for (std::size_t i = 0; i < out_left.size(); ++i) {
        INFO("frame " << i);
        REQUIRE(out_left[i] == Catch::Approx(want_left[i]).margin(1e-12));
        REQUIRE(out_right[i] == Catch::Approx(want_right[i]).margin(1e-12));
    }
}

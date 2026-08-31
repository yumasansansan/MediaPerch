// SPDX-License-Identifier: GPL-3.0-or-later
//
// Randomised invariants, in the spirit of a fuzzer but with no hardware, no
// files and no clang: these run in CI on every push, on every compiler.
//
// The hand-written tests elsewhere check the cases somebody thought of. These
// check the ones nobody did, by generating formats and sample data across the
// whole space and asserting the properties that must hold for all of them. The
// generator is a fixed-seed LCG, so a failure prints a seed that reproduces it
// exactly rather than "it went wrong once on the build server".

#include "mediaperch/negotiation.hpp"
#include "mediaperch/repack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

/// Deterministic, tiny, and the same on every platform -- which `std::mt19937`
/// is too, but this also generates the same stream for the same seed without
/// depending on which distribution the library implements how.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : state_(seed * 2 + 1) {}

    std::uint32_t next() noexcept
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33);
    }

    /// Inclusive.
    std::uint32_t between(std::uint32_t lo, std::uint32_t hi) noexcept
    {
        return lo + next() % (hi - lo + 1);
    }

    template <typename T, std::size_t N>
    T pick(const std::array<T, N>& from) noexcept
    {
        return from[next() % N];
    }

private:
    std::uint64_t state_;
};

constexpr std::array<mp::SampleType, 5> all_types{mp::SampleType::s16,
                                                  mp::SampleType::s24_packed,
                                                  mp::SampleType::s24_in_32,
                                                  mp::SampleType::s32,
                                                  mp::SampleType::f32};

constexpr std::array<mp::Encoding, 3> all_encodings{mp::Encoding::pcm, mp::Encoding::dop,
                                                    mp::Encoding::iec61937};

/// Deliberately includes rates and channel counts that no device has, and
/// occasionally something impossible, because `build_candidates` has to cope
/// with whatever a decoder reports rather than with what is reasonable.
mp::Format random_format(Rng& rng)
{
    static constexpr std::array<std::uint32_t, 12> rates{
        8000, 11025, 22050, 44100, 48000, 88200, 96000, 176400, 192000, 384000, 768000,
        1'048'575};

    mp::Format f;
    f.sample_rate = rng.next() % 20 == 0 ? 0 : rng.pick(rates);
    f.channels = rng.between(1, 9);
    f.sample_type = rng.pick(all_types);
    f.encoding = rng.pick(all_encodings);

    const std::uint32_t natural = mp::natural_valid_bits(f.sample_type);
    switch (rng.next() % 4) {
    case 0: f.valid_bits = 0; break;                       // "all of the container"
    case 1: f.valid_bits = natural; break;                 // the same thing, spelled out
    case 2: f.valid_bits = rng.between(1, natural); break; // fewer, which is legal
    default: f.valid_bits = rng.between(1, 40); break;     // sometimes impossible
    }

    switch (rng.next() % 3) {
    case 0: f.channel_mask = 0; break;
    case 1: f.channel_mask = mp::conventional_channel_mask(f.channels); break;
    default: f.channel_mask = rng.next() & 0xFFFFu; break; // usually the wrong popcount
    }
    return f;
}

} // namespace

TEST_CASE("every candidate is bit-exact, whatever the source", "[property]")
{
    for (std::uint64_t seed = 0; seed < 4000; ++seed) {
        Rng rng{seed};
        const mp::Format source = random_format(rng);
        const auto candidates = mp::build_candidates(source);

        if (!mp::is_valid(source)) {
            INFO("seed " << seed);
            CHECK(candidates.empty());
            continue;
        }

        INFO("seed " << seed << ": " << mp::describe(source));
        REQUIRE_FALSE(candidates.empty());

        // The source itself is always offered first and unchanged. A negotiator
        // that reorders this is asking a device to convert before it has been
        // asked whether it needs to.
        CHECK(candidates.front().format == source);
        CHECK(candidates.front().fidelity == mp::Fidelity::exact);

        for (const auto& c : candidates) {
            CHECK(mp::is_bit_exact(c.fidelity));
            CHECK(mp::is_valid(c.format));
            CHECK(c.format.sample_rate == source.sample_rate);
            CHECK(c.format.channels == source.channels);
            CHECK(c.format.encoding == source.encoding);
            // Nothing in the list may claim more or fewer real bits than the
            // source has: that is the whole definition of bit-exact.
            CHECK(mp::effective_valid_bits(c.format) == mp::effective_valid_bits(source));
            // And `classify` has to agree with the label the builder attached,
            // because the graph trusts `classify` and the tool prints the label.
            CHECK(mp::classify(source, c.format) == c.fidelity);
        }
    }
}

TEST_CASE("the candidate list never offers one wire format twice", "[property]")
{
    // s24_in_32 with 16 valid bits and s32 with 16 valid bits are the same
    // WAVEFORMATEXTENSIBLE. A duplicate costs a device round trip and hides a
    // modelling mistake.
    for (std::uint64_t seed = 0; seed < 4000; ++seed) {
        Rng rng{seed};
        const mp::Format source = random_format(rng);
        const auto candidates = mp::build_candidates(source);
        INFO("seed " << seed << ": " << mp::describe(source));

        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                const bool same_container =
                    mp::container_bytes(candidates[i].format.sample_type) ==
                    mp::container_bytes(candidates[j].format.sample_type);
                const bool same_mask =
                    candidates[i].format.channel_mask == candidates[j].format.channel_mask;
                CHECK_FALSE((same_container && same_mask));
            }
        }
    }
}

TEST_CASE("non-PCM encodings are never moved between containers", "[property]")
{
    for (std::uint64_t seed = 0; seed < 2000; ++seed) {
        Rng rng{seed};
        mp::Format source = random_format(rng);
        source.encoding = seed % 2 == 0 ? mp::Encoding::dop : mp::Encoding::iec61937;
        if (source.sample_type == mp::SampleType::f32) {
            source.sample_type = mp::SampleType::s24_in_32;
        }
        if (!mp::is_valid(source)) {
            continue;
        }

        INFO("seed " << seed << ": " << mp::describe(source));
        for (const auto& c : mp::build_candidates(source)) {
            // A DoP frame's markers sit in the top byte and a bitstream is not
            // samples; moving either one breaks it silently.
            CHECK(c.fidelity == mp::Fidelity::exact);
            CHECK(c.format.sample_type == source.sample_type);
        }
    }
}

TEST_CASE("repack round-trips for every pair of containers that fits", "[property]")
{
    constexpr std::array<mp::SampleType, 4> integer_types{
        mp::SampleType::s16, mp::SampleType::s24_packed, mp::SampleType::s24_in_32,
        mp::SampleType::s32};

    for (std::uint64_t seed = 0; seed < 3000; ++seed) {
        Rng rng{seed};
        const mp::SampleType from = rng.pick(integer_types);
        const mp::SampleType to = rng.pick(integer_types);
        const std::uint32_t from_bytes = mp::container_bytes(from);
        const std::uint32_t to_bytes = mp::container_bytes(to);
        const std::uint32_t valid =
            rng.between(1, std::min(from_bytes, to_bytes) * 8);

        constexpr std::size_t samples = 64;
        std::vector<std::uint8_t> original(samples * from_bytes);
        for (auto& byte : original) {
            byte = static_cast<std::uint8_t>(rng.next());
        }
        // Left-justified means the padding below the valid bits is zero. Random
        // bytes are not, so they are masked into a shape the format allows --
        // otherwise the round trip would be asked to preserve bits that a
        // conforming producer would never have set.
        const std::uint32_t pad_bits = from_bytes * 8 - valid;
        for (std::size_t i = 0; i < samples; ++i) {
            std::uint8_t* p = original.data() + i * from_bytes;
            for (std::uint32_t bit = 0; bit < pad_bits; ++bit) {
                p[bit / 8] = static_cast<std::uint8_t>(p[bit / 8] & ~(1u << (bit % 8)));
            }
        }

        INFO("seed " << seed << ": " << from_bytes << " bytes -> " << to_bytes
                     << " bytes, " << valid << " valid");

        std::vector<std::uint8_t> moved(samples * to_bytes);
        REQUIRE(mp::repack(original.data(), from, moved.data(), to, valid, samples));

        std::vector<std::uint8_t> back(samples * from_bytes);
        REQUIRE(mp::repack(moved.data(), to, back.data(), from, valid, samples));

        CHECK(back == original);
    }
}

TEST_CASE("repack refuses every pair that would lose signal", "[property]")
{
    constexpr std::array<mp::SampleType, 4> integer_types{
        mp::SampleType::s16, mp::SampleType::s24_packed, mp::SampleType::s24_in_32,
        mp::SampleType::s32};

    for (std::uint64_t seed = 0; seed < 2000; ++seed) {
        Rng rng{seed};
        const mp::SampleType from = rng.pick(integer_types);
        const mp::SampleType to = rng.pick(integer_types);
        const std::uint32_t valid = rng.between(1, 40);

        const std::vector<std::uint8_t> in(64 * 4, 0);
        std::vector<std::uint8_t> out(64 * 4, 0xCD);

        const bool fits = valid != 0 && valid <= mp::container_bytes(from) * 8 &&
                          valid <= mp::container_bytes(to) * 8;
        INFO("seed " << seed << ": " << valid << " valid bits");
        CHECK(mp::repack(in.data(), from, out.data(), to, valid, 64) == fits);
    }
}

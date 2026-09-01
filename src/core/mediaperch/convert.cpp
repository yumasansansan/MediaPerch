// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mp {
namespace {

/// Full scale for an integer container, as a magnitude.
///
/// 2^(bits-1), which is what every decoder in this tree divides by and what
/// every encoder multiplies by. It makes -1.0 exactly representable and +1.0 one
/// step past the top, which is the convention the whole format world uses and
/// the reason a clamp is needed rather than a scale of 2^(bits-1) - 1.
double full_scale(std::uint32_t bits) noexcept
{
    return std::exp2(static_cast<double>(bits) - 1.0);
}

/// One sample, as a number between -1 and 1. Reading is exact for every type.
double read_sample(const std::uint8_t* p, SampleType type) noexcept
{
    switch (type) {
    case SampleType::u8:
        return (static_cast<double>(*p) - 128.0) / 128.0;
    case SampleType::s16: {
        std::int16_t v = 0;
        std::memcpy(&v, p, 2);
        return static_cast<double>(v) / 32768.0;
    }
    case SampleType::s24_packed: {
        const auto raw = static_cast<std::int32_t>((static_cast<std::uint32_t>(p[2]) << 24) |
                                                   (static_cast<std::uint32_t>(p[1]) << 16) |
                                                   (static_cast<std::uint32_t>(p[0]) << 8));
        return static_cast<double>(raw >> 8) / 8388608.0;
    }
    case SampleType::s24_in_32:
    case SampleType::s32: {
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return static_cast<double>(v) / 2147483648.0;
    }
    case SampleType::f32: {
        float v = 0.0F;
        std::memcpy(&v, p, 4);
        return static_cast<double>(v);
    }
    case SampleType::f64: {
        double v = 0.0;
        std::memcpy(&v, p, 8);
        return v;
    }
    case SampleType::none:
        break;
    }
    return 0.0;
}

void write_sample(std::uint8_t* p, SampleType type, double value) noexcept
{
    switch (type) {
    case SampleType::u8:
        *p = static_cast<std::uint8_t>(static_cast<std::int32_t>(value) + 128);
        return;
    case SampleType::s16: {
        const auto v = static_cast<std::int16_t>(value);
        std::memcpy(p, &v, 2);
        return;
    }
    case SampleType::s24_packed: {
        const auto v = static_cast<std::int32_t>(value);
        p[0] = static_cast<std::uint8_t>(v & 0xFF);
        p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        return;
    }
    case SampleType::s24_in_32:
    case SampleType::s32: {
        const auto v = static_cast<std::int32_t>(value);
        std::memcpy(p, &v, 4);
        return;
    }
    case SampleType::f32: {
        const auto v = static_cast<float>(value);
        std::memcpy(p, &v, 4);
        return;
    }
    case SampleType::f64:
        std::memcpy(p, &value, 8);
        return;
    case SampleType::none:
        return;
    }
}

bool is_float(SampleType t) noexcept
{
    return t == SampleType::f32 || t == SampleType::f64;
}

} // namespace

Converter::Converter(const Format& from, const Format& to, ConvertConfig config) noexcept
    : from_(from), to_(to), config_(config), rng_(config.seed)
{
    possible_ = is_valid(from) && is_valid(to) && from.sample_rate == to.sample_rate &&
                from.channels == to.channels && from.encoding == Encoding::pcm &&
                to.encoding == Encoding::pcm && from.sample_type != SampleType::none &&
                to.sample_type != SampleType::none;
    if (!possible_) {
        return;
    }

    if (is_float(to.sample_type)) {
        // Float holds everything an integer container can, so nothing is lost
        // going this way -- unless a gain pushes it past what float represents,
        // which it does not at any gain anybody sets.
        scale_ = 1.0;
        ceiling_ = std::numeric_limits<double>::max();
        floor_ = -ceiling_;
        lossy_ = config.gain != 1.0;
        return;
    }

    // The destination's *valid* bits, not its container: 24 bits inside four
    // bytes is a 24-bit destination, and dithering it at 32 would put the noise
    // eight bits below where it belongs and do nothing.
    const std::uint32_t bits = effective_valid_bits(to);
    scale_ = full_scale(bits);
    ceiling_ = scale_ - 1.0;
    floor_ = -scale_;

    // A container that cannot hold the source exactly, or a float source, or a
    // gain: all three make this a conversion rather than a move.
    lossy_ = is_float(from.sample_type) || config.gain != 1.0 ||
             effective_valid_bits(from) > bits;

    // s24_in_32 and s32 are written as a whole 32-bit word, so a 24-bit value
    // has to be left-justified into it the way the container expects.
    if (to.sample_type == SampleType::s24_in_32 || to.sample_type == SampleType::s32) {
        const auto shift = std::exp2(static_cast<double>(32 - bits));
        scale_ = full_scale(bits) * shift;
        ceiling_ = std::exp2(31.0) - shift;
        floor_ = -std::exp2(31.0);
        // One step of the *destination's* least significant bit, which for 24
        // bits inside four bytes is 256 of the word rather than 1. Dithering at
        // the word's own LSB would put the noise eight bits too low and do
        // nothing at all.
        step_ = shift;
    }
}

double Converter::next_dither() noexcept
{
    // TPDF: the sum of two independent uniform values, which is the standard
    // choice because it decorrelates the error from the signal *and* keeps the
    // noise floor stationary. One uniform value would do the first and not the
    // second.
    auto uniform = [this]() noexcept {
        rng_ = rng_ * 1664525u + 1013904223u;
        return static_cast<double>(rng_) / 4294967296.0 - 0.5;
    };
    return uniform() + uniform();
}

void Converter::run(const void* src, void* dst, std::size_t frames) noexcept
{
    if (!possible_ || src == nullptr || dst == nullptr) {
        return;
    }

    const auto* in = static_cast<const std::uint8_t*>(src);
    auto* out = static_cast<std::uint8_t*>(dst);
    const std::size_t in_step = container_bytes(from_.sample_type);
    const std::size_t out_step = container_bytes(to_.sample_type);
    const std::size_t samples = frames * from_.channels;
    const bool to_float = is_float(to_.sample_type);
    const bool dithering = config_.dither && !to_float &&
                           (is_float(from_.sample_type) ||
                            effective_valid_bits(from_) > effective_valid_bits(to_));

    for (std::size_t i = 0; i < samples; ++i) {
        double v = read_sample(in + i * in_step, from_.sample_type) * config_.gain;

        if (to_float) {
            write_sample(out + i * out_step, to_.sample_type, v);
            continue;
        }

        v *= scale_;
        if (dithering) {
            v += next_dither() * step_;
        }
        // Round half away from zero, then clamp. The clamp is not paranoia: a
        // gain above unity, or a float source that legitimately exceeds full
        // scale -- which float WAV routinely does -- both land outside.
        v = std::round(v / step_) * step_;
        v = std::clamp(v, floor_, ceiling_);
        write_sample(out + i * out_step, to_.sample_type, v);
    }
}

} // namespace mp

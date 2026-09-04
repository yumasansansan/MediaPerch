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

std::uint32_t Converter::shaping_taps() const noexcept
{
    return dither_.empty() ? 0 : dither_.front().taps();
}

Converter::Converter(const Format& from, const Format& to, ConvertConfig config) noexcept
    : from_(from), to_(to), config_(config)
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
        // which it does not at any gain anybody sets. Nothing to dither and
        // nothing to shape: there is no quantiser here.
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

    // Dither and noise shaping earn their keep only where bits are being
    // thrown away. Widening cannot lose anything, so adding noise to it would
    // be vandalism.
    //
    // A gain counts, and leaving it out was a bug: 16 bits to 16 bits at 0.5
    // is not a widening, it is a quantiser whose input is half an LSB off the
    // grid on every other sample, and rounding that without dither is exactly
    // the correlated error this file exists to avoid.
    quantising_ = is_float(from.sample_type) || effective_valid_bits(from) > bits ||
                  config.gain != 1.0;

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

    // One generator per channel, each with its own seed, so that two channels
    // do not receive the same noise and turn it into a centre image.
    if (quantising_) {
        dither_.reserve(to.channels);
        for (std::uint32_t c = 0; c < to.channels; ++c) {
            dither_.emplace_back(config.dither, config.shaping, to.sample_rate,
                                 config.seed + c * 0x9E3779B9u);
        }
    }
}

void Converter::reset() noexcept
{
    for (Dither& noise : dither_) {
        noise.reset();
    }
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
    const unsigned channels = from_.channels;

    // Reading is exact for every type this handles and needs no comment beyond
    // saying so: binary64 has a 53-bit significand, every integer container
    // here is at most 32 bits, and the normalising divisor is a power of two,
    // which moves the exponent and leaves the significand alone. The only
    // rounding in this function is the one at the bottom of it.
    if (is_float(to_.sample_type)) {
        for (std::size_t i = 0; i < samples; ++i) {
            const double v = read_sample(in + i * in_step, from_.sample_type) * config_.gain;
            write_sample(out + i * out_step, to_.sample_type, v);
        }
        return;
    }

    for (std::size_t i = 0; i < samples; ++i) {
        const double v = read_sample(in + i * in_step, from_.sample_type) * config_.gain;

        // In LSBs of the destination from here down, which is the unit dither
        // and noise shaping are both defined in.
        const double lsb = v * scale_ / step_;

        double quantised = 0.0;
        double clamped = 0.0;
        bool clipped = false;
        if (quantising_) {
            Dither& noise = dither_[i % channels];
            // Add what the filter says this sample owes for the errors before
            // it, quantise, and hand back what this one cost. Adding rather
            // than subtracting is SSRC's convention and the one its curves are
            // written for; the other way round the same numbers shape the noise
            // *into* the midband.
            const double shaped = lsb + noise.feedback();
            quantised = std::round(shaped + noise.next());

            // The clamp is not paranoia: a gain above unity, a float source
            // that legitimately exceeds full scale -- which float WAV routinely
            // does -- and a shaper handed a transient all land outside.
            clamped = std::clamp(quantised * step_, floor_, ceiling_);
            clipped = clamped != quantised * step_;
            noise.accept(clamped / step_ - shaped, clipped);
        } else {
            quantised = std::round(lsb);
            clamped = std::clamp(quantised * step_, floor_, ceiling_);
        }

        write_sample(out + i * out_step, to_.sample_type, clamped);
    }
}

} // namespace mp

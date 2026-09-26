// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

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

/// Calls `f` with `t` as a compile-time constant, so that a loop over samples is
/// compiled once for each sample type -- with `read_sample` and `write_sample`
/// folded down to the one case -- rather than asking which type at every
/// sample.
template <class F>
void with_type(SampleType t, F&& f)
{
    switch (t) {
    case SampleType::u8:
        f(std::integral_constant<SampleType, SampleType::u8>{});
        return;
    case SampleType::s16:
        f(std::integral_constant<SampleType, SampleType::s16>{});
        return;
    case SampleType::s24_packed:
        f(std::integral_constant<SampleType, SampleType::s24_packed>{});
        return;
    case SampleType::s24_in_32:
        f(std::integral_constant<SampleType, SampleType::s24_in_32>{});
        return;
    case SampleType::s32:
        f(std::integral_constant<SampleType, SampleType::s32>{});
        return;
    case SampleType::f32:
        f(std::integral_constant<SampleType, SampleType::f32>{});
        return;
    case SampleType::f64:
        f(std::integral_constant<SampleType, SampleType::f64>{});
        return;
    case SampleType::none:
        return;
    }
}

/// What the quantiser needs, read once per call into locals: the bytes it
/// writes may be any object at all as far as the compiler knows, members of
/// the converter included, so a member read in the loop is read again after
/// every sample written.
struct Quantiser {
    double gain = 1.0;
    double scale = 1.0;
    double step = 1.0;
    /// 1 / step, exactly: the step is a power of two, so multiplying by this
    /// is the same division, without dividing.
    double per_step = 1.0;
    double floor = -1.0;
    double ceiling = 1.0;
};

/// Into a float destination: no quantiser, nothing to dither.
template <SampleType From, SampleType To>
void convert_to_float(const std::uint8_t* in, std::uint8_t* out, std::size_t samples,
                      double gain) noexcept
{
    const std::size_t in_step = container_bytes(From);
    const std::size_t out_step = container_bytes(To);
    for (std::size_t i = 0; i < samples; ++i) {
        write_sample(out + i * out_step, To, read_sample(in + i * in_step, From) * gain);
    }
}

/// Into an integer destination. `noise` is one generator per channel when the
/// conversion is quantising, and null when it is not.
template <SampleType From, SampleType To>
void convert_to_integer(const std::uint8_t* in, std::uint8_t* out, std::size_t frames,
                        unsigned channels, const Quantiser& q, Dither* noise) noexcept
{
    const std::size_t in_step = container_bytes(From);
    const std::size_t out_step = container_bytes(To);
    const double gain = q.gain;
    const double scale = q.scale;
    const double step = q.step;
    const double per_step = q.per_step;
    const double floor = q.floor;
    const double ceiling = q.ceiling;
    std::size_t i = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < channels; ++c, ++i) {
            const double v = read_sample(in + i * in_step, From) * gain;

            // In LSBs of the destination from here down, which is the unit
            // dither and noise shaping are both defined in.
            const double lsb = v * scale * per_step;

            double clamped = 0.0;
            if (noise != nullptr) {
                Dither& dither = noise[c];
                // Add what the filter says this sample owes for the errors
                // before it, quantise, and hand back what this one cost. Adding
                // rather than subtracting is SSRC's convention and the one its
                // curves are written for; the other way round the same numbers
                // shape the noise *into* the midband.
                const double shaped = lsb + dither.feedback();
                const double quantised = std::round(shaped + dither.next());

                // The clamp is not paranoia: a gain above unity, a float source
                // that legitimately exceeds full scale -- which float WAV
                // routinely does -- and a shaper handed a transient all land
                // outside.
                clamped = std::clamp(quantised * step, floor, ceiling);
                const bool clipped = clamped != quantised * step;
                dither.accept(clamped * per_step - shaped, clipped);
            } else {
                clamped = std::clamp(std::round(lsb) * step, floor, ceiling);
            }

            write_sample(out + i * out_step, To, clamped);
        }
    }
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
    const unsigned channels = from_.channels;

    // Reading is exact for every type this handles and needs no comment beyond
    // saying so: binary64 has a 53-bit significand, every integer container
    // here is at most 32 bits, and the normalising divisor is a power of two,
    // which moves the exponent and leaves the significand alone. The only
    // rounding in this function is the one at the bottom of it.
    if (is_float(to_.sample_type)) {
        const double gain = config_.gain;
        with_type(from_.sample_type, [&](auto from) {
            with_type(to_.sample_type, [&](auto to) {
                convert_to_float<decltype(from)::value, decltype(to)::value>(in, out,
                                                                             frames * channels,
                                                                             gain);
            });
        });
        return;
    }

    Quantiser q;
    q.gain = config_.gain;
    q.scale = scale_;
    q.step = step_;
    q.per_step = 1.0 / step_;
    q.floor = floor_;
    q.ceiling = ceiling_;
    Dither* noise = quantising_ ? dither_.data() : nullptr;
    with_type(from_.sample_type, [&](auto from) {
        with_type(to_.sample_type, [&](auto to) {
            convert_to_integer<decltype(from)::value, decltype(to)::value>(in, out, frames,
                                                                           channels, q, noise);
        });
    });
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/sine.hpp"

#include <cmath>
#include <cstring>
#include <numbers>
#include <numeric>

namespace mp {
namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;

/// Full scale for a container, as an integer, one bit short of overflow.
double full_scale(SampleType type) noexcept
{
    switch (type) {
    case SampleType::s16: return 32767.0;
    case SampleType::s24_packed: return 8388607.0;
    case SampleType::s24_in_32: return 8388607.0;
    case SampleType::s32: return 2147483647.0;
    case SampleType::f32: return 1.0;
    case SampleType::none: return 0.0;
    }
    return 0.0;
}

void write_le16(std::uint8_t* p, std::int32_t v) noexcept
{
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(u & 0xFFu);
    p[1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
}

void write_le24(std::uint8_t* p, std::int32_t v) noexcept
{
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(u & 0xFFu);
    p[1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
}

void write_le32(std::uint8_t* p, std::int32_t v) noexcept
{
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(u & 0xFFu);
    p[1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((u >> 24) & 0xFFu);
}

} // namespace

SineSource::SineSource(const Format& format, double hz, double amplitude)
    : format_(format), hz_(hz), amplitude_(amplitude)
{
    // When the frequency is a whole number of hertz the tone closes exactly over
    // rate/gcd frames, so the phase can be taken from a small reduced frame index
    // and an hour-long run is bit-identical to the first second of it.
    const double rounded = std::round(hz);
    if (format_.sample_rate != 0 && std::abs(hz - rounded) < 1e-9 && rounded > 0.0) {
        const auto whole = static_cast<std::uint64_t>(rounded);
        const auto rate = static_cast<std::uint64_t>(format_.sample_rate);
        period_frames_ = rate / std::gcd(rate, whole);
    } else {
        // Otherwise the phase resets once a second. For a test tone that costs a
        // discontinuity nobody is measuring; 1 kHz is what this exists for.
        period_frames_ = format_.sample_rate;
    }
    if (period_frames_ == 0) {
        period_frames_ = 1;
    }
}

void SineSource::write_frame(std::uint8_t* dst, std::int32_t value) const noexcept
{
    const std::uint32_t bytes = container_bytes(format_.sample_type);
    for (std::uint32_t ch = 0; ch < format_.channels; ++ch) {
        std::uint8_t* p = dst + static_cast<std::size_t>(ch) * bytes;
        switch (format_.sample_type) {
        case SampleType::s16:
            write_le16(p, value);
            break;
        case SampleType::s24_packed:
            write_le24(p, value);
            break;
        case SampleType::s24_in_32:
            // Left-justified, matching repack() and WAVEFORMATEXTENSIBLE.
            write_le32(p, static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 8));
            break;
        case SampleType::s32:
            write_le32(p, value);
            break;
        case SampleType::f32: {
            float f = 0.0F;
            std::memcpy(&f, &value, sizeof(f));
            std::memcpy(p, &f, sizeof(f));
            break;
        }
        case SampleType::none:
            break;
        }
    }
}

std::size_t SineSource::read(void* dst, std::size_t bytes)
{
    const std::uint32_t stride = frame_bytes(format_);
    if (stride == 0) {
        return 0;
    }

    const std::size_t frames = bytes / stride;
    auto* out = static_cast<std::uint8_t*>(dst);
    const double scale = full_scale(format_.sample_type) * amplitude_;
    const double rate = static_cast<double>(format_.sample_rate);

    for (std::size_t i = 0; i < frames; ++i) {
        const auto reduced = static_cast<double>((frame_ + i) % period_frames_);
        const double sample = std::sin(two_pi * hz_ * reduced / rate) * scale;

        std::int32_t value = 0;
        if (format_.sample_type == SampleType::f32) {
            const auto f = static_cast<float>(sample);
            std::memcpy(&value, &f, sizeof(value));
        } else {
            value = static_cast<std::int32_t>(std::lround(sample));
        }
        write_frame(out + i * stride, value);
    }

    frame_ += frames;
    return frames * stride;
}

} // namespace mp

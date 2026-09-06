// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/buffering.hpp"

#include <cmath>

namespace mp {
namespace {

struct Named {
    Dimension one;
    std::string_view name;
};

// The names a user types. Two, because two is what there is to measure; a
// third goes here and nowhere else.
constexpr Named k_names[] = {
    {Dimension::ring, "ring"},
    {Dimension::decoder_threads, "threads"},
};

} // namespace

std::string_view dimension_name(Dimension one) noexcept
{
    for (const Named& named : k_names) {
        if (named.one == one) {
            return named.name;
        }
    }
    return {};
}

bool dimension_from_name(std::string_view name, Dimension& out) noexcept
{
    for (const Named& named : k_names) {
        if (named.name == name) {
            out = named.one;
            return true;
        }
    }
    return false;
}

double samples_per_second(const StreamShape& shape) noexcept
{
    if (shape.width == 0 || shape.height == 0 || shape.fps_num == 0 || shape.fps_den == 0) {
        return 0.0;
    }
    return static_cast<double>(shape.width) * static_cast<double>(shape.height) *
           static_cast<double>(shape.fps_num) / static_cast<double>(shape.fps_den);
}

const Measurement* answer_for(const StreamShape& shape, const Profile& profile) noexcept
{
    const Measurement* exact = nullptr;
    const Measurement* cheapest_above = nullptr;
    const double want = samples_per_second(shape);

    for (const Measurement& one : profile.measured) {
        if (one.shape == shape) {
            exact = &one;
            break;
        }
        // A shape with no rate has no place on the axis. It can still be an
        // exact match -- two files of the same codec and geometry that both
        // timestamp every frame are the same class -- but it cannot stand in
        // for anything else, in either direction.
        if (want == 0.0) {
            continue;
        }
        const double cost = samples_per_second(one.shape);
        if (cost == 0.0 || cost < want) {
            continue;
        }
        if (cheapest_above == nullptr ||
            cost < samples_per_second(cheapest_above->shape)) {
            cheapest_above = &one;
        }
    }
    return exact != nullptr ? exact : cheapest_above;
}

std::uint32_t ring_for(const StreamShape& shape, const Profile& profile,
                       std::uint32_t period_frames, std::uint32_t sample_rate,
                       std::uint32_t fallback) noexcept
{
    if (period_frames == 0 || sample_rate == 0) {
        return fallback;
    }
    const Measurement* answer = answer_for(shape, profile);
    if (answer == nullptr || !(answer->ring_ms > 0.0)) {
        return fallback;
    }

    // Milliseconds back into periods, rounded up: a ring that is half a period
    // short of what was measured is a ring that was not measured.
    const double frames = answer->ring_ms * static_cast<double>(sample_rate) / 1000.0;
    const double periods = std::ceil(frames / static_cast<double>(period_frames));
    if (!(periods >= 1.0)) {
        return fallback;
    }
    // **Above the default as readily as below it.** A class that needs more
    // ring than the default is the one case a default cannot answer, and it is
    // the case a measurement answers best. Nothing is clamped: a ring the
    // machine will not give is a run that fails where the graph is built, which
    // is what §11 decided when `ring_periods` lost its range.
    if (periods > static_cast<double>(0xffffffffu)) {
        return 0xffffffffu;
    }
    return static_cast<std::uint32_t>(periods);
}

} // namespace mp

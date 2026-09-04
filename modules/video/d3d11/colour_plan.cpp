// SPDX-License-Identifier: GPL-3.0-or-later
#include "colour_plan.hpp"

#include <cstring>

namespace mp::video {

const char* name_of(ToneMap m) noexcept
{
    switch (m) {
    case ToneMap::none:
        return "none";
    case ToneMap::driver:
        return "driver";
    case ToneMap::d2d:
        return "d2d";
    case ToneMap::shader:
        return "shader";
    }
    return "none";
}

const char* name_of(SwapFormat f) noexcept
{
    switch (f) {
    case SwapFormat::fp16_scrgb:
        return "fp16 scRGB";
    case SwapFormat::rgb10_hdr10:
        return "rgb10 HDR10";
    }
    return "fp16 scRGB";
}

bool tone_map_from_name(const char* name, ToneMap& out) noexcept
{
    if (name == nullptr) {
        return false;
    }
    struct Named {
        const char* name;
        ToneMap value;
    };
    static constexpr Named k_names[] = {
        {"none", ToneMap::none},
        {"driver", ToneMap::driver},
        {"d2d", ToneMap::d2d},
        {"shader", ToneMap::shader},
    };
    for (const Named& named : k_names) {
        if (std::strcmp(name, named.name) == 0) {
            out = named.value;
            return true;
        }
    }
    return false;
}

} // namespace mp::video

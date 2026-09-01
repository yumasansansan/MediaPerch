// SPDX-License-Identifier: GPL-3.0-or-later

#include "autoeq.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace mp::autoeq {
namespace {

/// Equalizer APO's filter types, and what this program calls them.
///
/// AutoEq emits PK, LSC and HSC; the rest turn up in files people have edited
/// by hand, and refusing those would be refusing a file that is not wrong. LS
/// and LSC differ in how a shelf's steepness is specified, and both arrive here
/// with a Q, so both map to the same shelf.
bool kind_of(const std::string& token, mp::biquad::Kind& out)
{
    if (token == "PK" || token == "PEQ" || token == "Modal") {
        out = mp::biquad::Kind::peak;
    } else if (token == "LS" || token == "LSC" || token == "LSQ") {
        out = mp::biquad::Kind::lowshelf;
    } else if (token == "HS" || token == "HSC" || token == "HSQ") {
        out = mp::biquad::Kind::highshelf;
    } else if (token == "LP" || token == "LPQ") {
        out = mp::biquad::Kind::lowpass;
    } else if (token == "HP" || token == "HPQ") {
        out = mp::biquad::Kind::highpass;
    } else if (token == "BP") {
        out = mp::biquad::Kind::bandpass;
    } else if (token == "NO") {
        out = mp::biquad::Kind::notch;
    } else if (token == "AP") {
        out = mp::biquad::Kind::allpass;
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) {
            ++at;
        }
        const std::size_t start = at;
        while (at < line.size() && !std::isspace(static_cast<unsigned char>(line[at]))) {
            ++at;
        }
        if (at > start) {
            out.push_back(line.substr(start, at - start));
        }
    }
    return out;
}

/// The value after `key` in a `Fc 105 Hz Gain 4.2 dB Q 0.70` run.
bool value_after(const std::vector<std::string>& tokens, const char* key, double& out)
{
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] != key) {
            continue;
        }
        const char* start = tokens[i + 1].c_str();
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start || !std::isfinite(value)) {
            return false;
        }
        out = value;
        return true;
    }
    return false;
}

bool parse_graphic(const std::string& text, Profile& out, std::string& why)
{
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        why = "a GraphicEQ line has a colon after the word";
        return false;
    }
    std::size_t at = colon + 1;
    while (at <= text.size()) {
        const std::size_t next = text.find(';', at);
        const std::string pair =
            text.substr(at, next == std::string::npos ? next : next - at);
        const std::vector<std::string> parts = words(pair);
        if (parts.size() == 2) {
            const char* a = parts[0].c_str();
            const char* b = parts[1].c_str();
            char* end_a = nullptr;
            char* end_b = nullptr;
            const double hz = std::strtod(a, &end_a);
            const double db = std::strtod(b, &end_b);
            if (end_a == a || end_b == b || !std::isfinite(hz) || !std::isfinite(db) ||
                hz <= 0.0) {
                why = "`" + pair + "` is not a frequency and a level";
                return false;
            }
            out.curve.emplace_back(hz, db);
        } else if (!parts.empty()) {
            why = "`" + pair + "` is not a frequency and a level";
            return false;
        }
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    if (out.curve.size() < 2) {
        why = "a curve needs at least two points";
        return false;
    }
    std::sort(out.curve.begin(), out.curve.end());
    out.kind = "graphic";
    return true;
}

} // namespace

bool parse(const std::string& text, Profile& out, std::string& why)
{
    out = Profile{};
    if (text.find("GraphicEQ") != std::string::npos) {
        return parse_graphic(text, out, why);
    }

    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find('\n', at);
        std::string line = text.substr(at, next == std::string::npos ? next : next - at);
        at = next == std::string::npos ? text.size() + 1 : next + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> tokens = words(line);
        if (tokens.empty()) {
            continue;
        }
        if (tokens[0] == "Preamp:") {
            if (tokens.size() < 2) {
                why = "could not read the preamp from `" + line + "`";
                return false;
            }
            const char* start = tokens[1].c_str();
            char* end = nullptr;
            const double value = std::strtod(start, &end);
            if (end == start || !std::isfinite(value)) {
                why = "could not read the preamp from `" + line + "`";
                return false;
            }
            out.preamp_db = value;
            continue;
        }
        if (tokens[0] != "Filter") {
            // Comments and headers are not errors; a line that looks like a
            // filter and is not one is.
            continue;
        }
        // `Filter 1: ON PK Fc 105 Hz Gain 4.2 dB Q 0.70`
        std::size_t i = 1;
        while (i < tokens.size() && tokens[i].find(':') == std::string::npos) {
            ++i;
        }
        ++i;
        if (i >= tokens.size()) {
            why = "`" + line + "` stops before it says anything";
            return false;
        }
        const bool enabled = tokens[i] != "OFF";
        if (tokens[i] == "ON" || tokens[i] == "OFF") {
            ++i;
        }
        if (i >= tokens.size()) {
            why = "`" + line + "` has no filter type";
            return false;
        }
        mp::biquad::Band band;
        band.enabled = enabled;
        if (!kind_of(tokens[i], band.kind)) {
            // "None" is Equalizer APO's way of writing an empty slot.
            if (tokens[i] == "None") {
                continue;
            }
            why = "`" + tokens[i] + "` is not a filter type this reads";
            return false;
        }
        const std::vector<std::string> rest(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                                            tokens.end());
        if (!value_after(rest, "Fc", band.frequency_hz)) {
            why = "`" + line + "` has no centre frequency";
            return false;
        }
        // Gain and Q are absent on the kinds that have no use for them.
        (void)value_after(rest, "Gain", band.gain_db);
        (void)value_after(rest, "Q", band.q);
        if (band.q <= 0.0) {
            band.q = 0.70710678118654752;
        }
        out.bands.push_back(band);
    }

    if (out.bands.empty()) {
        why = "no filters and no curve: this is not a profile this reads";
        return false;
    }
    out.kind = "parametric";
    return true;
}

bool load(const std::string& path, Profile& out, std::string& why)
{
    std::FILE* file = nullptr;
#if defined(_WIN32)
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide <= 0) {
        why = "that path is not UTF-8";
        return false;
    }
    std::vector<wchar_t> name(static_cast<std::size_t>(wide));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, name.data(), wide);
    if (_wfopen_s(&file, name.data(), L"rb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        why = "could not open " + path;
        return false;
    }

    std::string text;
    char buffer[4096];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got == 0) {
            break;
        }
        text.append(buffer, got);
        if (text.size() > (1u << 22)) {
            std::fclose(file);
            why = "that file is far too large to be an equaliser profile";
            return false;
        }
    }
    std::fclose(file);
    return parse(text, out, why);
}

double curve_db(const std::vector<std::pair<double, double>>& curve, double hz) noexcept
{
    if (curve.empty()) {
        return 0.0;
    }
    if (hz <= curve.front().first) {
        return curve.front().second;
    }
    if (hz >= curve.back().first) {
        return curve.back().second;
    }
    // Logarithmic in frequency, linear in decibels -- which is the axis the
    // points were placed on and the one hearing works on.
    const auto after = std::lower_bound(
        curve.begin(), curve.end(), hz,
        [](const std::pair<double, double>& point, double value) {
            return point.first < value;
        });
    const auto before = after - 1;
    const double span = std::log(after->first) - std::log(before->first);
    if (span <= 0.0) {
        return before->second;
    }
    const double t = (std::log(hz) - std::log(before->first)) / span;
    return before->second + t * (after->second - before->second);
}

} // namespace mp::autoeq

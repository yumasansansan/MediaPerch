// SPDX-License-Identifier: GPL-3.0-or-later

#include "cube.hpp"

#include <cmath>
#include <cstdlib>

namespace mp {
namespace {

/// One line, without its ending. `\r` goes too, because a file written on
/// Windows and read on Linux is the ordinary case and a stray carriage return
/// would land inside the last number on every line.
std::string_view line_at(std::string_view text, std::size_t& at)
{
    const std::size_t start = at;
    while (at < text.size() && text[at] != '\n') {
        ++at;
    }
    std::size_t end = at;
    if (end > start && text[end - 1] == '\r') {
        --end;
    }
    if (at < text.size()) {
        ++at; // past the newline
    }
    return text.substr(start, end - start);
}

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// The next whitespace-separated word, or empty at the end.
std::string_view word(std::string_view s, std::size_t& at)
{
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) {
        ++at;
    }
    const std::size_t start = at;
    while (at < s.size() && s[at] != ' ' && s[at] != '\t') {
        ++at;
    }
    return s.substr(start, at - start);
}

/// **Finite, or not a number at all.** A LUT with an infinity in it is a LUT
/// that produces one, and a NaN survives every arithmetic downstream and shows
/// up as a black pixel nobody can explain.
bool number(std::string_view text, float& out)
{
    if (text.empty()) {
        return false;
    }
    const std::string held{text};
    char* end = nullptr;
    const double value = std::strtod(held.c_str(), &end);
    if (end != held.c_str() + held.size() || !std::isfinite(value)) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool three(std::string_view rest, float out[3])
{
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        if (!number(word(rest, at), out[i])) {
            return false;
        }
    }
    return word(rest, at).empty(); // a fourth number is not a triplet
}

std::string said(std::string_view name, std::size_t line, const std::string& what)
{
    return std::string{name} + ":" + std::to_string(line) + ": " + what;
}

} // namespace

bool CubeLut::identity(float tolerance) const noexcept
{
    if (size < 2 || table.size() != static_cast<std::size_t>(size) * size * size * 3) {
        return false;
    }
    // The identity is only the identity over 0..1: a table whose domain says
    // otherwise maps that domain onto itself, which is a different function.
    for (int c = 0; c < 3; ++c) {
        if (domain_min[c] != 0.0f || domain_max[c] != 1.0f) {
            return false;
        }
    }
    const auto step = 1.0f / static_cast<float>(size - 1);
    std::size_t at = 0;
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                const float want[3] = {static_cast<float>(r) * step,
                                       static_cast<float>(g) * step,
                                       static_cast<float>(b) * step};
                for (int c = 0; c < 3; ++c) {
                    if (std::fabs(table[at + static_cast<std::size_t>(c)] - want[c]) >
                        tolerance) {
                        return false;
                    }
                }
                at += 3;
            }
        }
    }
    return true;
}

CubeLut identity_cube(std::uint32_t size)
{
    CubeLut out;
    if (size < 2 || size > k_cube_max_size) {
        return out;
    }
    out.size = size;
    out.table.resize(static_cast<std::size_t>(size) * size * size * 3);
    const auto step = 1.0f / static_cast<float>(size - 1);
    std::size_t at = 0;
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                out.table[at++] = static_cast<float>(r) * step;
                out.table[at++] = static_cast<float>(g) * step;
                out.table[at++] = static_cast<float>(b) * step;
            }
        }
    }
    return out;
}

CubeText parse_cube(std::string_view text, std::string_view name)
{
    CubeText out;
    std::size_t at = 0;
    std::size_t line = 0;
    std::size_t entries = 0; // triplets read so far

    while (at < text.size()) {
        ++line;
        const std::string_view raw = trimmed(line_at(text, at));
        if (raw.empty() || raw.front() == '#') {
            continue;
        }

        std::size_t cursor = 0;
        const std::string_view key = word(raw, cursor);
        const std::string_view rest = trimmed(raw.substr(cursor));

        if (key == "TITLE") {
            // Quoted in every file anybody writes, and the quotes are not part
            // of it. A title that is not quoted is taken as it stands rather
            // than refused: the format's own examples are inconsistent.
            std::string_view value = rest;
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            out.lut.title = std::string{value};
            continue;
        }
        if (key == "LUT_1D_SIZE") {
            out.why = said(name, line,
                           "this is a 1D LUT, which is three curves rather than a cube; "
                           "nothing here applies one");
            return out;
        }
        if (key == "LUT_3D_SIZE") {
            if (out.lut.size != 0) {
                out.why = said(name, line, "LUT_3D_SIZE is stated twice");
                return out;
            }
            float said_size = 0.0f;
            std::size_t only = 0;
            if (!number(word(rest, only), said_size) || !word(rest, only).empty() ||
                said_size < 2.0f || said_size > static_cast<float>(k_cube_max_size) ||
                said_size != std::floor(said_size)) {
                out.why = said(name, line,
                               "LUT_3D_SIZE is a whole number from 2 to " +
                                   std::to_string(k_cube_max_size));
                return out;
            }
            out.lut.size = static_cast<std::uint32_t>(said_size);
            // **Allocated only now, and only this much.** The bound above is
            // what stands between a two-line file and a memory bomb.
            out.lut.table.resize(static_cast<std::size_t>(out.lut.size) * out.lut.size *
                                 out.lut.size * 3);
            continue;
        }
        if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            float* into = key == "DOMAIN_MIN" ? out.lut.domain_min : out.lut.domain_max;
            if (!three(rest, into)) {
                out.why = said(name, line, std::string{key} + " is three numbers");
                return out;
            }
            continue;
        }
        // Anything else is a row of the table, or it is nothing this reads.
        if (out.lut.size == 0) {
            out.why = said(name, line,
                           "a table row before LUT_3D_SIZE, so there is no table to put "
                           "it in");
            return out;
        }
        if (entries * 3 >= out.lut.table.size()) {
            out.why = said(name, line, "more rows than LUT_3D_SIZE says there are");
            return out;
        }
        float triplet[3] = {0.0f, 0.0f, 0.0f};
        std::size_t row = 0;
        if (!number(key, triplet[0]) || !number(word(rest, row), triplet[1]) ||
            !number(word(rest, row), triplet[2]) || !word(rest, row).empty()) {
            out.why = said(name, line, "a table row is three finite numbers");
            return out;
        }
        out.lut.table[entries * 3 + 0] = triplet[0];
        out.lut.table[entries * 3 + 1] = triplet[1];
        out.lut.table[entries * 3 + 2] = triplet[2];
        ++entries;
    }

    if (out.lut.size == 0) {
        out.why = std::string{name} + ": no LUT_3D_SIZE in it, so it is not a cube LUT";
        return out;
    }
    if (entries * 3 != out.lut.table.size()) {
        // **Short is refused rather than padded.** A table with the last plane
        // missing is a table whose brightest colours are black, and a reader
        // that filled them in would be inventing a transform.
        out.why = std::string{name} + ": " + std::to_string(entries) + " rows for a size " +
                  std::to_string(out.lut.size) + " cube, which needs " +
                  std::to_string(out.lut.table.size() / 3);
        return out;
    }
    for (int c = 0; c < 3; ++c) {
        if (!(out.lut.domain_min[c] < out.lut.domain_max[c])) {
            out.why = std::string{name} +
                      ": DOMAIN_MIN is not below DOMAIN_MAX, so the domain is empty";
            return out;
        }
    }

    out.ok = true;
    return out;
}

} // namespace mp

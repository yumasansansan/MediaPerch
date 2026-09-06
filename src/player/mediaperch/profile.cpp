// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/profile.hpp"

#include "mediaperch/result.hpp"

#include "dragonperch/ini.hpp"

#include <charconv>
#include <cstdio>

namespace mp {
namespace {

std::string say(std::string_view name, std::size_t line, const std::string& what)
{
    return std::string{name} + ":" + std::to_string(line) + ": " + what;
}

/// `from_chars` with the whole value consumed, because "3840x" is not 3840 and
/// a profile that read it as one would be a profile that invented a class.
bool whole_number(std::string_view text, std::uint64_t& out) noexcept
{
    if (text.empty()) {
        return false;
    }
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto got = std::from_chars(first, last, value, base);
    if (got.ec != std::errc{} || got.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

bool whole_number(std::string_view text, std::uint32_t& out) noexcept
{
    std::uint64_t wide = 0;
    if (!whole_number(text, wide) || wide > 0xffffffffULL) {
        return false;
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
}

/// `24000/1001`, and `24000` for a rate somebody wrote without a denominator.
bool ratio(std::string_view text, std::uint32_t& num, std::uint32_t& den) noexcept
{
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        if (!whole_number(text, num)) {
            return false;
        }
        den = 1;
        return true;
    }
    return whole_number(dp::ini::trim(text.substr(0, slash)), num) &&
           whole_number(dp::ini::trim(text.substr(slash + 1)), den);
}

/// A double, and only a finite one. A ring of `inf` milliseconds is not a
/// measurement anybody took.
bool milliseconds(const std::string& text, double& out) noexcept
{
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || !(value >= 0.0) || value > 1e9) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

/// Neither `#` nor `;`, which the parser reads as the start of a comment
/// wherever they appear.
bool writable_as_a_value(std::string_view text) noexcept
{
    return text.find('#') == std::string_view::npos &&
           text.find(';') == std::string_view::npos;
}

} // namespace

ProfileText parse_profile(std::string_view text, std::string_view name)
{
    ProfileText out;
    const auto sections = dp::ini::parse(text, dp::ini::OnBadLine::skip);

    for (const dp::ini::Section& section : sections) {
        if (section.name != "measurement") {
            // Not an error. A profile may be shared with a build that writes
            // more than this one reads, and saying so costs nothing.
            out.complaints.push_back(
                say(name, section.line,
                    "nothing reads a section called `" + section.name +
                        "`; this build reads [measurement]"));
            continue;
        }

        Measurement one;
        std::string trouble;
        for (const dp::ini::Entry& entry : section.entries) {
            bool read = true;
            if (entry.key == "codec") {
                std::uint32_t codec = 0;
                read = whole_number(entry.value, codec);
                one.shape.codec = static_cast<MpCodec>(codec);
            } else if (entry.key == "width") {
                read = whole_number(entry.value, one.shape.width);
            } else if (entry.key == "height") {
                read = whole_number(entry.value, one.shape.height);
            } else if (entry.key == "fps") {
                read = ratio(entry.value, one.shape.fps_num, one.shape.fps_den);
            } else if (entry.key == "ring_ms") {
                read = milliseconds(entry.value, one.ring_ms);
            } else if (entry.key == "threads") {
                read = whole_number(entry.value, one.decoder_threads);
            } else if (entry.key == "runs") {
                read = whole_number(entry.value, one.runs);
            } else if (entry.key == "file") {
                one.file = entry.value;
            } else {
                out.complaints.push_back(
                    say(name, entry.line,
                        "there is no measurement key called `" + entry.key + "`"));
                continue;
            }
            if (!read && trouble.empty()) {
                trouble = say(name, entry.line,
                              "`" + entry.key + " = " + entry.value +
                                  "` is not a number this can use, so the whole "
                                  "measurement is skipped");
            }
        }

        if (!trouble.empty()) {
            // **The section goes, not the key.** A class whose geometry read and
            // whose ring did not is a class with no answer in it, and keeping
            // half of one would put a zero where a measurement should be.
            out.complaints.push_back(trouble);
            continue;
        }
        if (one.shape.width == 0 || one.shape.height == 0) {
            out.complaints.push_back(
                say(name, section.line,
                    "a measurement with no picture size in it names no class of "
                    "stream, so it is skipped"));
            continue;
        }
        out.profile.measured.push_back(std::move(one));
    }
    return out;
}

std::string write_profile(const Profile& profile)
{
    std::string out;
    out += "# MediaPerch: what this machine measured.\n";
    out += "#\n";
    out += "# Written by the calibration and read as *defaults*. Anything set\n";
    out += "# explicitly -- a flag, or a key in the settings file -- beats every\n";
    out += "# number here, and deleting this file only means the defaults come\n";
    out += "# back. It is not the settings file and is not edited in its place.\n";
    out += "#\n";
    out += "# A ring is in milliseconds because a device period is not the same\n";
    out += "# length on two machines, and this file is about one machine.\n";

    for (const Measurement& one : profile.measured) {
        char line[160];
        std::snprintf(line, sizeof line, "\n# %s %ux%u\n", codec_name(one.shape.codec),
                      static_cast<unsigned>(one.shape.width),
                      static_cast<unsigned>(one.shape.height));
        out += line;
        out += "[measurement]\n";
        std::snprintf(line, sizeof line, "codec = 0x%08x\n",
                      static_cast<unsigned>(one.shape.codec));
        out += line;
        out += "width = " + std::to_string(one.shape.width) + "\n";
        out += "height = " + std::to_string(one.shape.height) + "\n";
        if (one.shape.fps_num != 0 && one.shape.fps_den != 0) {
            out += "fps = " + std::to_string(one.shape.fps_num) + "/" +
                   std::to_string(one.shape.fps_den) + "\n";
        }
        std::snprintf(line, sizeof line, "ring_ms = %.3f\n", one.ring_ms);
        out += line;
        if (one.decoder_threads != 0) {
            out += "threads = " + std::to_string(one.decoder_threads) + "\n";
        }
        if (one.runs != 0) {
            out += "runs = " + std::to_string(one.runs) + "\n";
        }
        // Left out rather than mangled: see the header.
        if (!one.file.empty() && writable_as_a_value(one.file)) {
            out += "file = " + one.file + "\n";
        }
    }
    return out;
}

} // namespace mp

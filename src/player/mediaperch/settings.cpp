// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/settings.hpp"

#include "dragonperch/ini.hpp"

#include <string>

namespace mp {
namespace {

std::string say(std::string_view name, std::size_t line, const std::string& what)
{
    return std::string{name} + ":" + std::to_string(line) + ": " + what;
}

} // namespace

std::vector<ipc::Setting> engine_settings(const Settings& settings)
{
    const auto joined = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& one : items) {
            if (!out.empty()) {
                out += ',';
            }
            out += one;
        }
        return out;
    };
    const auto row = [](const char* key, std::string value, const char* what,
                        const char* spec) {
        ipc::Setting out{key, std::move(value), what};
        (void)ipc::parse_setting_spec(spec, out);
        return out;
    };
    return {
        row("pipe", settings.pipe,
            "where the engine listens; empty is this platform's usual name", "text"),
        row("modules", settings.modules,
            "where modules are loaded from; empty is beside the executable",
            "path pick=folder"),
        row("allow", joined(settings.allow), "module ids that may load; empty is all of them",
            "text"),
        row("decoders", joined(settings.decoders),
            "container readers to try before the scores decide, in this order. "
            "A reordering and not a veto (\u00a77)",
            "text"),
        row("profile", settings.profile,
            "this machine's buffering profile; empty is profile.ini beside the "
            "settings file",
            "path"),
    };
}

bool set_engine(Settings& settings, const std::string& key, const std::string& value,
                std::string& why)
{
    if (key == "pipe") {
        settings.pipe = value;
    } else if (key == "modules") {
        settings.modules = value;
    } else if (key == "allow") {
        settings.allow = split_list(value);
    } else if (key == "decoders") {
        settings.decoders = split_list(value);
    } else if (key == "profile") {
        settings.profile = value;
    } else {
        why = "there is no engine setting called `" + key + "`";
        return false;
    }
    return true;
}

std::vector<std::string> split_list(std::string_view text)
{
    std::vector<std::string> out;
    for (std::size_t at = 0; at <= text.size();) {
        const std::size_t next = std::min(text.find(',', at), text.size());
        const std::string_view piece = dp::ini::trim(text.substr(at, next - at));
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        at = next + 1;
    }
    return out;
}

SettingsFile read_settings(std::string_view text, std::string_view name)
{
    SettingsFile out;
    // Skip rather than refuse: this is a file people edit by hand.
    const auto sections = dp::ini::parse(text, dp::ini::OnBadLine::skip);

    for (const dp::ini::Section& section : sections) {
        if (section.name == "player") {
            for (const dp::ini::Entry& entry : section.entries) {
                // Not checked here. What a player setting means is `Player::set`
                // and there is one of those; a second opinion in this file would
                // be a second schema to keep in step.
                out.settings.player.push_back(
                    PlayerSetting{entry.key, entry.value, entry.line});
            }
            continue;
        }
        if (section.name == "engine") {
            for (const dp::ini::Entry& entry : section.entries) {
                if (entry.key == "pipe") {
                    out.settings.pipe = entry.value;
                } else if (entry.key == "modules") {
                    out.settings.modules = entry.value;
                } else if (entry.key == "allow") {
                    out.settings.allow = split_list(entry.value);
                } else if (entry.key == "decoders") {
                    out.settings.decoders = split_list(entry.value);
                } else if (entry.key == "profile") {
                    out.settings.profile = entry.value;
                } else {
                    out.complaints.push_back(
                        say(name, entry.line, "there is no engine setting called `" +
                                                  entry.key + "`"));
                }
            }
            continue;
        }
        // A section nobody reads is more likely a typo than a plan, and saying
        // so costs nothing. It is not an error: a file may be shared with a
        // version of this program that has more sections than this one.
        out.complaints.push_back(
            say(name, section.line,
                "nothing reads a section called `" + section.name +
                    "`; the ones this build knows are [engine] and [player]"));
    }
    return out;
}

std::string write_settings(const Settings& settings)
{
    const auto list = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) {
            if (!out.empty()) {
                out += ", ";
            }
            out += item;
        }
        return out;
    };

    std::string out;
    out += "# MediaPerch settings.\n";
    out += "#\n";
    out += "# Written by the engine and meant to be edited by hand. A line it cannot\n";
    out += "# read is skipped and complained about in the log, so one typo costs one\n";
    out += "# setting rather than all of them.\n";
    out += "#\n";
    out += "# Every key under [player] is a `mediaperch-cli set` key, because two ways\n";
    out += "# of saying the same thing that were not the same thing would be worse\n";
    out += "# than either. `mediaperch-cli settings` lists them with what they mean.\n";
    out += "\n[engine]\n";
    out += "# Where the engine listens. Empty is this platform's usual name.\n";
    out += "pipe = " + settings.pipe + "\n";
    out += "# Where modules are loaded from. Empty is beside the executable.\n";
    out += "modules = " + settings.modules + "\n";
    out += "# Module ids that may be loaded. Empty is all of them.\n";
    out += "allow = " + list(settings.allow) + "\n";
    out += "# Decoders to try before the scores decide, in this order.\n";
    out += "decoders = " + list(settings.decoders) + "\n";
    out += "# This machine's buffering profile, from `mediaperch-probe calibrate`.\n";
    out += "# Empty is profile.ini beside this file.\n";
    out += "profile = " + settings.profile + "\n";
    out += "\n[player]\n";
    for (const PlayerSetting& setting : settings.player) {
        out += setting.key + " = " + setting.value + "\n";
    }
    return out;
}

} // namespace mp

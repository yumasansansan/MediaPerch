// SPDX-License-Identifier: GPL-3.0-or-later
//
// One file, read by the head, meaning the same thing on a platform that does not
// exist yet.
//
// **The schema is not here.** What a player setting means is decided by
// `Player::set`, in exactly one place, and this file's `[player]` section is a
// list of arguments to it. A settings file and `mediaperch-cli set` that
// disagreed about what `path = bitexact` meant would be worse than either, and
// the only way to be sure they cannot is for there to be one of them.
//
// What *is* here is the part a player cannot own: where to listen, where the
// modules are, which of them may be loaded, and which decoder to prefer. Those
// are needed before there is a player at all.
//
// **The parser is DragonPerch's**, as a submodule, because the alternative was a
// second INI parser with a second set of edge cases and a second fuzz corpus.
// Its `OnBadLine::skip` is what this uses and its own header comment says why:
// for a file people edit by hand, losing every setting to one typo is worse
// than losing the setting the typo is in.
//
// One consequence worth knowing before it surprises somebody: `#` and `;` start
// a comment anywhere on a line, including after a value, so a setting whose
// value needs one of those characters cannot have it. Nothing here does.

#ifndef MEDIAPERCH_SETTINGS_HPP
#define MEDIAPERCH_SETTINGS_HPP

#include <cstddef>
#include "mediaperch/protocol.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mp {

/// One `key = value` from `[player]`, with the line it came from so a complaint
/// can name it.
struct PlayerSetting {
    std::string key;
    std::string value;
    std::size_t line = 0;
};

struct Settings {
    /// Where the engine listens. Empty means the platform's default name.
    std::string pipe;
    /// Where modules are loaded from. Empty means beside the executable.
    std::string modules;
    /// Module ids that may be loaded. Empty means all of them -- an allow-list
    /// that is empty by default is a list nobody has decided to have.
    std::vector<std::string> allow;
    /// Container readers to prefer, in this order, ahead of the ranked list.
    /// How somebody says "read MP4s with FFmpeg" without arguing with the
    /// scores. **A reordering, not a veto**: a module named here that does not
    /// recognise a file is not a reason to refuse the file.
    std::vector<std::string> decoders;
    /// Where this machine's buffering profile is (§9.8.2). Empty means the
    /// one beside this file, which is where `calibrate` writes it.
    ///
    /// **In `[engine]` rather than `[player]`, because it is not a setting.**
    /// The keys under `[player]` are arguments to `Player::set` and a person
    /// can change any of them while something is playing; this names a file to
    /// read at startup, which is the same kind of thing as where the modules
    /// are.
    std::string profile;
    /// Everything else, in the order the file gave it, for `Player::set`.
    std::vector<PlayerSetting> player;
};

struct SettingsFile {
    Settings settings;
    /// Lines that could not be used, already worded for a log. Never fatal.
    std::vector<std::string> complaints;
};

/// The `[engine]` half as rows a shell shows, in the same shape `[player]`'s
/// take -- key, value, and what it means.
///
/// **Listed here rather than assembled by a shell**, for the reason §11 gives
/// about the file: what a setting means belongs in one place, and a second
/// list of these would be a second place to keep in step.
[[nodiscard]] std::vector<ipc::Setting> engine_settings(const Settings& settings);

/// Applies one. False and a reason when the key is not one of them, or the
/// value is not one it takes.
///
/// **Every one of these takes effect at the next start**, which is why they are
/// not `Player::set` keys: the pipe is bound, the modules are scanned and the
/// profile is read before there is a player, and a setting that silently did
/// nothing until a restart would be worse than one that says so.
[[nodiscard]] bool set_engine(Settings& settings, const std::string& key,
                              const std::string& value, std::string& why);

/// Reads settings from the text of a file. `name` appears in complaints.
[[nodiscard]] SettingsFile read_settings(std::string_view text,
                                         std::string_view name = "settings");

/// The file that would produce these settings, with the comments a person
/// opening it needs. Round-trips: `read_settings(write_settings(s))` gives back
/// `s`, which is the only reason a program is allowed to write a file somebody
/// else edits.
[[nodiscard]] std::string write_settings(const Settings& settings);

/// A comma-separated list, trimmed, with the empties dropped. Exposed because
/// more than one caller splits a value this way and two rules would differ.
[[nodiscard]] std::vector<std::string> split_list(std::string_view text);

} // namespace mp

#endif // MEDIAPERCH_SETTINGS_HPP

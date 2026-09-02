// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings file, fed bytes nobody wrote on purpose.
//
// It is a small parser reading a file a person edits, which is exactly the
// combination that gets skipped: too simple to look dangerous, and reachable by
// anything that can drop a file in `%APPDATA%`. Thirty seconds of libFuzzer on
// every push is cheap insurance against that judgement being wrong.
//
// The corpus is DragonPerch's, not a second one of ours. The parser is shared
// as a submodule for the same reason: two INI parsers in one house would have
// two sets of edge cases and only one of them would be fuzzed.

#include "mediaperch/settings.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const mp::SettingsFile file = mp::read_settings(text, "fuzz");

    // Touch everything it produced, so that a length or an index that is wrong
    // is wrong somewhere ASan can see it rather than in a field nobody read.
    std::size_t bytes = file.settings.pipe.size() + file.settings.modules.size();
    for (const std::string& item : file.settings.allow) {
        bytes += item.size();
    }
    for (const std::string& item : file.settings.decoders) {
        bytes += item.size();
    }
    for (const mp::PlayerSetting& setting : file.settings.player) {
        bytes += setting.key.size() + setting.value.size() + setting.line;
    }
    for (const std::string& complaint : file.complaints) {
        bytes += complaint.size();
    }

    // And what it writes has to be readable again -- a settings file this
    // program cannot read back is not a settings file, and the round trip is
    // the only claim `write_settings` actually makes.
    const std::string written = mp::write_settings(file.settings);
    const mp::SettingsFile again = mp::read_settings(written, "fuzz");
    if (again.settings.player.size() != file.settings.player.size()) {
        __builtin_trap();
    }
    return static_cast<int>(bytes & 0);
}

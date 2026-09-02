// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings file: what it means, and what it does with a file somebody got
// wrong.
//
// The second half is most of it. This is a file people edit by hand, so the
// interesting behaviour is not "a correct file reads correctly" but "a file with
// one bad line loses one setting" -- and the only way to know that is still true
// is to write the bad files down.

#include "mediaperch/settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

const mp::PlayerSetting* find(const mp::Settings& settings, const std::string& key)
{
    const auto found = std::find_if(settings.player.begin(), settings.player.end(),
                                    [&](const mp::PlayerSetting& s) { return s.key == key; });
    return found == settings.player.end() ? nullptr : &*found;
}

bool complained_about(const mp::SettingsFile& file, const std::string& fragment)
{
    return std::any_of(file.complaints.begin(), file.complaints.end(),
                       [&](const std::string& line) {
                           return line.find(fragment) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("a settings file says what it says", "[settings]")
{
    const auto file = mp::read_settings(R"(# MediaPerch settings.
[engine]
pipe = \\.\pipe\somewhere
modules = C:\modules
allow = mp_sink_wasapi, decode_native
decoders = decode_flac, decode_native

[player]
device = KA5
path = auto
gain = 0.5
)");

    CHECK(file.complaints.empty());
    CHECK(file.settings.pipe == "\\\\.\\pipe\\somewhere");
    CHECK(file.settings.modules == "C:\\modules");
    CHECK(file.settings.allow == std::vector<std::string>{"mp_sink_wasapi", "decode_native"});
    CHECK(file.settings.decoders == std::vector<std::string>{"decode_flac", "decode_native"});

    REQUIRE(file.settings.player.size() == 3);
    const mp::PlayerSetting* device = find(file.settings, "device");
    REQUIRE(device != nullptr);
    CHECK(device->value == "KA5");
    // The line, because a complaint about a setting has to be able to name it.
    CHECK(device->line == 9);
}

TEST_CASE("what a player setting means is not decided here", "[settings]")
{
    // `path = sideways` is nonsense, and this file passes it through anyway.
    // The schema is `Player::set`, in one place; a second opinion here would be
    // a second schema to keep in step with the first.
    const auto file = mp::read_settings("[player]\npath = sideways\n");
    CHECK(file.complaints.empty());
    REQUIRE(file.settings.player.size() == 1);
    CHECK(file.settings.player.front().value == "sideways");
}

TEST_CASE("one bad line costs one setting", "[settings]")
{
    // The whole reason this is `OnBadLine::skip`: losing every setting to one
    // typo is worse than losing the setting the typo is in.
    const auto file = mp::read_settings(R"([player]
device = KA5
this line is not a setting at all
gain = 0.25
)");
    REQUIRE(file.settings.player.size() == 2);
    CHECK(find(file.settings, "device") != nullptr);
    CHECK(find(file.settings, "gain") != nullptr);
}

TEST_CASE("a settings file complains where it can", "[settings]")
{
    SECTION("an engine key nobody reads")
    {
        const auto file = mp::read_settings("[engine]\nvolume = 11\n", "settings.ini");
        CHECK(complained_about(file, "no engine setting called `volume`"));
        // Named with its line, because a complaint that does not is a search.
        CHECK(complained_about(file, "settings.ini:2:"));
    }

    SECTION("a section nobody reads")
    {
        const auto file = mp::read_settings("[video]\ntonemap = reinhard\n");
        CHECK(complained_about(file, "nothing reads a section called `video`"));
    }

    SECTION("and it is still not fatal")
    {
        const auto file = mp::read_settings("[nonsense]\n[player]\ngain = 2\n");
        CHECK(file.complaints.size() == 1);
        CHECK(find(file.settings, "gain") != nullptr);
    }
}

TEST_CASE("a repeated key is the last one", "[settings]")
{
    // What somebody means by putting a corrected line under the old one is the
    // correction. The entries are kept in order and applied in order, so the
    // last one is what the player ends up with.
    const auto file = mp::read_settings("[player]\ngain = 1\ngain = 2\n");
    REQUIRE(file.settings.player.size() == 2);
    CHECK(file.settings.player.front().value == "1");
    CHECK(file.settings.player.back().value == "2");
}

TEST_CASE("both comment characters are comments", "[settings]")
{
    // `#` is what people type and `;` is what KConfig writes. This file is meant
    // to be readable by a settings program that does not exist yet.
    const auto file = mp::read_settings("# one\n; two\n[player]\ngain = 1 # and this\n");
    CHECK(file.complaints.empty());
    REQUIRE(file.settings.player.size() == 1);
    // Including at the end of a value, which is the price: a setting whose
    // value needs a `#` or a `;` in it cannot have one. No setting this program
    // has does, and a comment somebody wrote and the parser ignored would be
    // the more surprising of the two behaviours.
    CHECK(file.settings.player.front().value == "1");
}

TEST_CASE("nothing in is nothing out", "[settings]")
{
    for (const std::string_view text : {"", "\n\n\n", "# only a comment\n", "\xff\xfe\x00\x01"}) {
        const auto file = mp::read_settings(text);
        CHECK(file.settings.player.empty());
        CHECK(file.settings.pipe.empty());
    }
}

TEST_CASE("a settings file this program wrote is one it can read", "[settings]")
{
    // The only claim `write_settings` makes. A program that writes a file
    // somebody else edits and cannot read its own output has written a trap.
    mp::Settings settings;
    settings.pipe = "\\\\.\\pipe\\mediaperch";
    settings.modules = "C:\\Program Files\\MediaPerch";
    settings.allow = {"mp_sink_wasapi", "mp_decode_native"};
    settings.decoders = {"decode_flac"};
    settings.player = {{"device", "FiiO KA5", 0},
                       {"path", "bitexact", 0},
                       {"dsp", "resample:rate=96000,eq", 0},
                       {"gain", "1.000000", 0},
                       {"shaping", "shibata:5", 0}};

    const auto again = mp::read_settings(mp::write_settings(settings));
    CHECK(again.complaints.empty());
    CHECK(again.settings.pipe == settings.pipe);
    CHECK(again.settings.modules == settings.modules);
    CHECK(again.settings.allow == settings.allow);
    CHECK(again.settings.decoders == settings.decoders);
    REQUIRE(again.settings.player.size() == settings.player.size());
    for (std::size_t i = 0; i < settings.player.size(); ++i) {
        INFO(settings.player[i].key);
        CHECK(again.settings.player[i].key == settings.player[i].key);
        CHECK(again.settings.player[i].value == settings.player[i].value);
    }
}

TEST_CASE("an empty list is empty rather than one empty thing", "[settings]")
{
    CHECK(mp::split_list("").empty());
    CHECK(mp::split_list("   ").empty());
    CHECK(mp::split_list(",,,").empty());
    CHECK(mp::split_list("one") == std::vector<std::string>{"one"});
    CHECK(mp::split_list(" one , two ,, three ") ==
          std::vector<std::string>{"one", "two", "three"});
}

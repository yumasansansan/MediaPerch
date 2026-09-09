// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the engine and a shell say to each other, and what happens when one of
// them says something else.
//
// **The second half is the point.** A shell is another process, possibly
// somebody else's, possibly compiled against a different version of this
// header, possibly hostile. Every test here that feeds the reader nonsense is a
// test that the engine says no rather than believing a length.

#include "mediaperch/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

mp::Format cd_audio()
{
    return mp::Format{.sample_rate = 44100,
                      .channels = 2,
                      .channel_mask = 0x3,
                      .sample_type = mp::SampleType::s16,
                      .encoding = mp::Encoding::pcm,
                      .valid_bits = 16};
}

} // namespace

TEST_CASE("a status survives the round trip", "[ipc][protocol]")
{
    mp::ipc::Status sent;
    sent.state = mp::ipc::State::playing;
    sent.index = 3;
    sent.count = 12;
    sent.position = 1234567;
    sent.length = 9876543;
    sent.track = "C:/music/\xe3\x83\x96\xe3\x83\xab\xe3\x83\xbc.flac"; // UTF-8, on purpose
    sent.decoder = "demux_wav";
    sent.device = "\xe3\x83\x98\xe3\x83\x83\xe3\x83\x89\xe3\x83\x9b\xe3\x83\xb3 (FiiO KA5)";
    sent.source = cd_audio();
    sent.wire = cd_audio();
    sent.wire.sample_type = mp::SampleType::s32;
    sent.fidelity = 2;
    sent.processed = true;
    sent.frames_rendered = 44100 * 60;
    sent.underruns = 0;
    sent.error = "";

    mp::ipc::Writer w;
    write(w, sent);
    const auto message = mp::ipc::frame(mp::ipc::Kind::status_reply, 42, w);

    mp::ipc::Header header{};
    REQUIRE(mp::ipc::parse_header(message.data(), message.size(), header));
    CHECK(header.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));
    CHECK(header.id == 42);
    CHECK(header.payload == w.size());
    CHECK(message.size() == mp::ipc::k_header_bytes + header.payload);

    mp::ipc::Reader r{message.data() + mp::ipc::k_header_bytes, header.payload};
    mp::ipc::Status got;
    REQUIRE(read(r, got));
    CHECK(r.complete()); // everything read, nothing left over

    CHECK(got.state == sent.state);
    CHECK(got.index == sent.index);
    CHECK(got.count == sent.count);
    CHECK(got.position == sent.position);
    CHECK(got.length == sent.length);
    CHECK(got.track == sent.track);
    CHECK(got.decoder == sent.decoder);
    CHECK(got.device == sent.device);
    CHECK(got.source == sent.source);
    CHECK(got.wire == sent.wire);
    CHECK(got.fidelity == sent.fidelity);
    CHECK(got.processed == sent.processed);
    CHECK(got.frames_rendered == sent.frames_rendered);
    CHECK(got.underruns == sent.underruns);
}

TEST_CASE("lists survive the round trip", "[ipc][protocol]")
{
    SECTION("a playlist")
    {
        const std::vector<std::string> sent{"a.flac", "", "a path with spaces.wav"};
        mp::ipc::Writer w;
        mp::ipc::write_strings(w, sent);
        mp::ipc::Reader r{w.bytes()};
        std::vector<std::string> got;
        REQUIRE(mp::ipc::read_strings(r, got));
        CHECK(r.complete());
        CHECK(got == sent);
    }

    SECTION("a settings tree")
    {
        // Every kind, with everything a row can carry, because a field added
        // to a row inside a counted list is exactly what an old reader takes
        // for the next row.
        std::vector<mp::ipc::Setting> sent{
            {"path", "bitexact", "what may happen to the samples"},
            {"device", "", "or empty for the default"}};
        mp::ipc::Setting up;
        up.key = "up";
        up.value = "lanczos";
        up.description = "the kernel";
        REQUIRE(mp::ipc::parse_setting_spec("enum:bilinear,hermite,lanczos group=Upscaling", up));
        sent.push_back(up);
        mp::ipc::Setting lobes;
        lobes.key = "up_lobes";
        lobes.value = "3";
        lobes.description = "how many";
        REQUIRE(mp::ipc::parse_setting_spec("int min=1 step=1 group=Upscaling when=up=lanczos",
                                            lobes));
        sent.push_back(lobes);
        for (const char* spec : {"number step=0.05 unit=dB", "bool", "size", "path pick=folder"}) {
            mp::ipc::Setting one;
            one.key = spec;
            one.value = "x";
            REQUIRE(mp::ipc::parse_setting_spec(spec, one));
            sent.push_back(one);
        }
        mp::ipc::Setting cost;
        cost.key = "cost";
        cost.value = "12";
        cost.description = "taps (read only)";
        cost.read_only = true;
        sent.push_back(cost);

        mp::ipc::Writer w;
        write(w, sent);
        mp::ipc::Reader r{w.bytes()};
        std::vector<mp::ipc::Setting> got;
        REQUIRE(read(r, got));
        CHECK(r.complete());
        REQUIRE(got.size() == sent.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            INFO(sent[i].key);
            CHECK(got[i].key == sent[i].key);
            CHECK(got[i].value == sent[i].value);
            CHECK(got[i].description == sent[i].description);
            CHECK(got[i].read_only == sent[i].read_only);
            CHECK(got[i].kind == sent[i].kind);
            CHECK(got[i].choices == sent[i].choices);
            CHECK(got[i].group == sent[i].group);
            CHECK(got[i].when == sent[i].when);
            CHECK(got[i].hints == sent[i].hints);
        }
        CHECK(got[2].kind == mp::ipc::SettingKind::choice);
        CHECK(got[2].choices == std::vector<std::string>{"bilinear", "hermite", "lanczos"});
        CHECK(got[3].kind == mp::ipc::SettingKind::integer);
        CHECK(got[3].when == "up=lanczos");
        CHECK(got[3].hints == "min=1 step=1");
        CHECK(got[4].kind == mp::ipc::SettingKind::number);
        CHECK(got[4].hints == "step=0.05 unit=dB");
        CHECK(got[5].kind == mp::ipc::SettingKind::toggle);
        CHECK(got[6].kind == mp::ipc::SettingKind::size);
        CHECK(got[7].kind == mp::ipc::SettingKind::path);
        CHECK(got[7].hints == "pick=folder");
        CHECK(got[8].read_only);
        CHECK(got[8].kind == mp::ipc::SettingKind::text);

        // A row cut short anywhere inside its new fields is refused, not read
        // as a shorter row.
        const std::vector<std::uint8_t>& whole = w.bytes();
        for (std::size_t cut : {whole.size() - 1, whole.size() - 3, whole.size() - 9}) {
            mp::ipc::Reader short_r{whole.data(), cut};
            std::vector<mp::ipc::Setting> partial;
            CHECK_FALSE(read(short_r, partial));
        }
    }
}

TEST_CASE("numbers keep their exact value", "[ipc][protocol]")
{
    // Through the bits rather than through text: a double that survives a round
    // trip as decimal is a double somebody rounded.
    mp::ipc::Writer w;
    w.f64(0.1);
    w.f64(-1.0 / 3.0);
    w.i64(-9007199254740993LL);
    w.u64(~std::uint64_t{0});
    mp::ipc::Reader r{w.bytes()};
    CHECK(r.f64() == 0.1);
    CHECK(r.f64() == -1.0 / 3.0);
    CHECK(r.i64() == -9007199254740993LL);
    CHECK(r.u64() == ~std::uint64_t{0});
    CHECK(r.complete());
}

TEST_CASE("the wire is little-endian whatever built it", "[ipc][protocol]")
{
    // Written out by hand, because "it round-trips" would also be true of a
    // format that meant something different on the other machine.
    mp::ipc::Writer w;
    w.u32(0x01020304u);
    const auto& b = w.bytes();
    REQUIRE(b.size() == 4);
    CHECK(b[0] == 0x04);
    CHECK(b[1] == 0x03);
    CHECK(b[2] == 0x02);
    CHECK(b[3] == 0x01);
}

TEST_CASE("a header that is not ours is refused", "[ipc][protocol]")
{
    const mp::ipc::Writer empty;
    auto message = mp::ipc::frame(mp::ipc::Kind::status, 1, empty);
    REQUIRE(message.size() == mp::ipc::k_header_bytes);
    mp::ipc::Header header{};

    SECTION("the right one is accepted, so the rest of this means something")
    {
        CHECK(mp::ipc::parse_header(message.data(), message.size(), header));
    }
    SECTION("wrong magic")
    {
        message[0] ^= 0xFFu;
        CHECK_FALSE(mp::ipc::parse_header(message.data(), message.size(), header));
    }
    SECTION("a version this build does not speak")
    {
        // Not "read it anyway and hope": the fields may have moved, and two
        // processes agreeing on a length while disagreeing on everything else
        // is the bug this refusal exists to prevent.
        message[4] = static_cast<std::uint8_t>(mp::ipc::k_version + 1);
        CHECK_FALSE(mp::ipc::parse_header(message.data(), message.size(), header));
    }
    SECTION("a payload longer than any message is allowed to be")
    {
        message[12] = 0xFFu;
        message[13] = 0xFFu;
        message[14] = 0xFFu;
        message[15] = 0xFFu;
        CHECK_FALSE(mp::ipc::parse_header(message.data(), message.size(), header));
    }
    SECTION("not even a whole header")
    {
        CHECK_FALSE(mp::ipc::parse_header(message.data(), message.size() - 1, header));
        CHECK_FALSE(mp::ipc::parse_header(nullptr, 0, header));
    }
}

TEST_CASE("a reader stops at the end and stays stopped", "[ipc][protocol]")
{
    // Poisoned rather than throwing, so that a caller may decode a whole
    // message and ask once at the end -- which is the only way that check
    // actually gets written.
    mp::ipc::Writer w;
    w.u32(7);
    mp::ipc::Reader r{w.bytes()};
    CHECK(r.u32() == 7);
    CHECK(r.ok());
    CHECK(r.done());
    CHECK(r.u32() == 0);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.done());
    CHECK(r.str().empty());
    CHECK_FALSE(r.ok());
}

TEST_CASE("a length nobody can back up is refused", "[ipc][protocol]")
{
    SECTION("a string that claims more than the message holds")
    {
        // The one field in a message that costs something on its own. Checked
        // before it is trusted with an allocation.
        mp::ipc::Writer w;
        w.u32(0xFFFFFFFFu); // a length
        w.u8(1);            // and one byte to back it up
        mp::ipc::Reader r{w.bytes()};
        CHECK(r.str().empty());
        CHECK_FALSE(r.ok());
    }

    SECTION("a list that claims more items than could be there")
    {
        mp::ipc::Writer w;
        w.u32(0xFFFFFFFFu);
        mp::ipc::Reader r{w.bytes()};
        std::vector<std::string> items;
        CHECK_FALSE(mp::ipc::read_strings(r, items));
        CHECK(items.empty());
    }

    SECTION("a list whose count is honest but whose items are not")
    {
        mp::ipc::Writer w;
        w.u32(3);
        w.str("one");
        mp::ipc::Reader r{w.bytes()};
        std::vector<std::string> items;
        CHECK_FALSE(mp::ipc::read_strings(r, items));
    }
}

TEST_CASE("a truncated status is not half-read", "[ipc][protocol]")
{
    mp::ipc::Status sent;
    sent.state = mp::ipc::State::paused;
    sent.track = "something";
    mp::ipc::Writer w;
    write(w, sent);

    for (std::size_t cut = 1; cut < w.size(); cut += 3) {
        mp::ipc::Reader r{w.bytes().data(), cut};
        mp::ipc::Status got;
        INFO("cut at " << cut);
        // Every prefix of a message is either rejected or complete; there is no
        // length at which a partial message reads as a whole one.
        CHECK((read(r, got) == false || r.complete()));
    }
}

TEST_CASE("every kind has a name", "[ipc][protocol]")
{
    // Not decoration: these go into logs and error messages, and a number in a
    // log is a table lookup somebody has to do by hand.
    const mp::ipc::Kind kinds[] = {
        mp::ipc::Kind::hello,          mp::ipc::Kind::status,
        mp::ipc::Kind::play,           mp::ipc::Kind::enqueue,
        mp::ipc::Kind::clear,          mp::ipc::Kind::pause,
        mp::ipc::Kind::resume,         mp::ipc::Kind::stop,
        mp::ipc::Kind::seek,           mp::ipc::Kind::next,
        mp::ipc::Kind::previous,       mp::ipc::Kind::playlist,
        mp::ipc::Kind::settings,       mp::ipc::Kind::setting_set,
        mp::ipc::Kind::log,            mp::ipc::Kind::subscribe,
        mp::ipc::Kind::quit,           mp::ipc::Kind::ok,
        mp::ipc::Kind::error,          mp::ipc::Kind::hello_reply,
        mp::ipc::Kind::status_reply,   mp::ipc::Kind::playlist_reply,
        mp::ipc::Kind::settings_reply, mp::ipc::Kind::log_reply,
        mp::ipc::Kind::event_state,    mp::ipc::Kind::event_log,
    };
    for (const mp::ipc::Kind kind : kinds) {
        INFO("kind " << static_cast<int>(kind));
        CHECK(std::string{mp::ipc::kind_name(kind)} != "unknown");
    }
    CHECK(std::string{mp::ipc::kind_name(static_cast<mp::ipc::Kind>(9999))} == "unknown");
    CHECK(std::string{mp::ipc::state_name(mp::ipc::State::stopped)} == "stopped");
}

TEST_CASE("a calibration survives the wire", "[protocol]")
{
    // **Every field is a choice somebody made about their own afternoon.** A
    // calibration cannot run faster than the material, so a field lost on the
    // wire is an hour spent measuring something nobody asked for.
    mp::ipc::Calibration sent;
    sent.files = {"C:/clips/forest.mkv", "D:/a file with spaces.mp4"};
    sent.dimensions = 3;
    sent.sweep = 2;
    sent.windows = 5;
    sent.window_seconds = 12.5;
    sent.start_ring = 128;
    sent.lowest_ring = 16;
    sent.highest_ring = 4096;

    mp::ipc::Writer w;
    mp::ipc::write(w, sent);

    mp::ipc::Reader r{w.bytes()};
    mp::ipc::Calibration got;
    REQUIRE(mp::ipc::read(r, got));
    CHECK(r.complete());
    CHECK(got.files == sent.files);
    CHECK(got.dimensions == sent.dimensions);
    CHECK(got.sweep == sent.sweep);
    CHECK(got.windows == sent.windows);
    CHECK(got.window_seconds == sent.window_seconds);
    CHECK(got.start_ring == sent.start_ring);
    CHECK(got.lowest_ring == sent.lowest_ring);
    CHECK(got.highest_ring == sent.highest_ring);
}

TEST_CASE("the calibration verbs have names, because a trace is read by a person",
          "[protocol]")
{
    using mp::ipc::Kind;
    CHECK(std::string{mp::ipc::kind_name(Kind::calibrate)} == "calibrate");
    CHECK(std::string{mp::ipc::kind_name(Kind::profile)} == "profile");
    CHECK(std::string{mp::ipc::kind_name(Kind::profile_reply)} == "profile_reply");
}

TEST_CASE("a truncated calibration is refused rather than half-read", "[protocol]")
{
    mp::ipc::Calibration sent;
    sent.files = {"one.mkv"};
    mp::ipc::Writer w;
    mp::ipc::write(w, sent);

    std::vector<std::uint8_t> cut = w.bytes();
    cut.resize(cut.size() - 3);
    mp::ipc::Reader r{cut};
    mp::ipc::Calibration got;
    CHECK_FALSE(mp::ipc::read(r, got));
}

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return in ? text.str() : std::string{};
}

/// The `name = number` lines of the enum whose declaration starts with
/// `heading`, with the names normalised -- lower case, underscores dropped --
/// so that `node_setting_set` and `NodeSettingSet` are one key.
std::map<std::string, long> enum_table(const std::string& text, const std::string& heading)
{
    std::map<std::string, long> out;
    const std::size_t start = text.find(heading);
    if (start == std::string::npos) {
        return out;
    }
    std::istringstream lines{text.substr(start)};
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        if (!inside) {
            inside = line.find('{') != std::string::npos;
            continue;
        }
        const std::size_t close = line.find('}');
        const std::size_t comment = line.find("//");
        const std::string code = line.substr(0, std::min(close, comment));
        const std::size_t equals = code.find('=');
        if (equals != std::string::npos) {
            std::string name;
            for (const char c : code.substr(0, equals)) {
                if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                    name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            const char* digits = code.c_str() + equals + 1;
            char* end = nullptr;
            const long value = std::strtol(digits, &end, 10);
            if (!name.empty() && end != digits) {
                out[name] = value;
            }
        }
        if (close != std::string::npos) {
            break;
        }
    }
    return out;
}

long number_after(const std::string& text, const std::string& what)
{
    const std::size_t at = text.find(what);
    return at == std::string::npos ? -1 : std::strtol(text.c_str() + at + what.size(), nullptr, 10);
}

} // namespace

TEST_CASE("the shell's C# mirror of the wire agrees with the C++ enum", "[ipc][protocol]")
{
    // **A hand-written mirror needs a test that reads the mirror.** The wire
    // is described twice on purpose -- a serialiser would be a third
    // description -- and the second description was wrong in the one way a
    // trace cannot show: `Save` and `Quit` were each other's number in the
    // shell, so "Save settings" quit the engine and closing the window asked
    // it to save. This reads both files and holds every enumerator of `Kind`
    // and `NodeKind`, and the version, to one number.
    const std::string cpp = slurp(MEDIAPERCH_SOURCE_DIR "/src/player/mediaperch/protocol.hpp");
    const std::string cs = slurp(MEDIAPERCH_SOURCE_DIR "/shell/winui/Ipc/Protocol.cs");
    REQUIRE(!cpp.empty());
    REQUIRE(!cs.empty());

    for (const char* which : {"Kind", "NodeKind", "SettingKind"}) {
        INFO(which);
        const auto ours = enum_table(cpp, std::string{"\nenum class "} + which + " :");
        const auto theirs = enum_table(cs, std::string{"\npublic enum "} + which + " :");
        REQUIRE(!ours.empty());
        REQUIRE(!theirs.empty());
        for (const auto& [name, value] : ours) {
            INFO(name);
            const auto it = theirs.find(name);
            REQUIRE(it != theirs.end());
            CHECK(it->second == value);
        }
        for (const auto& [name, value] : theirs) {
            INFO(name);
            CHECK(ours.count(name) == 1u);
        }
    }
    CHECK(number_after(cpp, "k_version = ") == number_after(cs, "Version = "));
    CHECK(number_after(cpp, "k_version = ") == static_cast<long>(mp::ipc::k_version));
}

TEST_CASE("a describe row's fourth field says what kind of value it takes", "[ipc][protocol]")
{
    // **The grammar every module writes in, read once.** A module's row is
    // `key\tvalue\thelp[\tspec]`; the spec is a type and then hints. What a
    // shell draws depends on this being read the same way for every module,
    // which is why it is read here and not in each of three places.
    mp::ipc::Setting row;

    SECTION("three fields is a text row, as every row was")
    {
        REQUIRE(mp::ipc::setting_from_row("gain\t1.5\tlinear gain", row));
        CHECK(row.key == "gain");
        CHECK(row.value == "1.5");
        CHECK(row.description == "linear gain");
        CHECK_FALSE(row.read_only);
        CHECK(row.kind == mp::ipc::SettingKind::text);
        CHECK(row.choices.empty());
        CHECK(row.group.empty());
    }
    SECTION("two fields is a row with no help")
    {
        REQUIRE(mp::ipc::setting_from_row("gain\t1.5", row));
        CHECK(row.value == "1.5");
        CHECK(row.description.empty());
        CHECK(row.kind == mp::ipc::SettingKind::text);
    }
    SECTION("one field is not a row")
    {
        CHECK_FALSE(mp::ipc::setting_from_row("gain", row));
    }
    SECTION("the marker is still read, and the help does not include the spec")
    {
        REQUIRE(mp::ipc::setting_from_row("peak\t0.5\tloudest (read only)", row));
        CHECK(row.read_only);
        REQUIRE(mp::ipc::setting_from_row("up\tlanczos\tthe kernel\tenum:hermite,lanczos group=Up",
                                          row));
        CHECK_FALSE(row.read_only);
        CHECK(row.description == "the kernel");
        CHECK(row.kind == mp::ipc::SettingKind::choice);
        CHECK(row.choices == std::vector<std::string>{"hermite", "lanczos"});
        CHECK(row.group == "Up");
    }
    SECTION("every type")
    {
        CHECK(mp::ipc::parse_setting_spec("text", row));
        CHECK(row.kind == mp::ipc::SettingKind::text);
        CHECK(mp::ipc::parse_setting_spec("int", row));
        CHECK(row.kind == mp::ipc::SettingKind::integer);
        CHECK(mp::ipc::parse_setting_spec("number", row));
        CHECK(row.kind == mp::ipc::SettingKind::number);
        CHECK(mp::ipc::parse_setting_spec("bool", row));
        CHECK(row.kind == mp::ipc::SettingKind::toggle);
        CHECK(mp::ipc::parse_setting_spec("size", row));
        CHECK(row.kind == mp::ipc::SettingKind::size);
        CHECK(mp::ipc::parse_setting_spec("path", row));
        CHECK(row.kind == mp::ipc::SettingKind::path);
        CHECK(mp::ipc::parse_setting_spec("enum:a", row));
        CHECK(row.kind == mp::ipc::SettingKind::choice);
        CHECK(row.choices == std::vector<std::string>{"a"});
    }
    SECTION("hints keep their order and their words; group and when are lifted out")
    {
        REQUIRE(mp::ipc::parse_setting_spec(
            "int  min=1 max=1000 step=1 unit=lobes group=Upscaling when=up=lanczos,spline36",
            row));
        CHECK(row.kind == mp::ipc::SettingKind::integer);
        CHECK(row.hints == "min=1 max=1000 step=1 unit=lobes");
        CHECK(row.group == "Upscaling");
        CHECK(row.when == "up=lanczos,spline36");
    }
    SECTION("a spec this cannot read is a text row and says so")
    {
        // A previous spec's fields do not leak into a row with a bad one.
        REQUIRE(mp::ipc::parse_setting_spec("enum:a,b group=G", row));
        for (const char* bad : {"enum", "enum:", "float", "int min", "int =3", "int min=",
                                "int when=lanczos", "int when==x", "int when=up="}) {
            INFO(bad);
            CHECK_FALSE(mp::ipc::parse_setting_spec(bad, row));
            CHECK(row.kind == mp::ipc::SettingKind::text);
            CHECK(row.choices.empty());
            CHECK(row.group.empty());
            CHECK(row.when.empty());
            CHECK(row.hints.empty());
            // Through the row as well: the key, value and help are still read.
            REQUIRE(mp::ipc::setting_from_row(std::string{"k\tv\th\t"} + bad, row));
            CHECK(row.key == "k");
            CHECK(row.description == "h");
            CHECK(row.kind == mp::ipc::SettingKind::text);
        }
        CHECK_FALSE(mp::ipc::parse_setting_spec("", row));
        CHECK(row.kind == mp::ipc::SettingKind::text);
    }
    SECTION("the names a tool prints")
    {
        CHECK(std::string{mp::ipc::setting_kind_name(mp::ipc::SettingKind::choice)} == "choice");
        CHECK(std::string{mp::ipc::setting_kind_name(mp::ipc::SettingKind::toggle)} == "toggle");
        CHECK(std::string{mp::ipc::setting_kind_name(mp::ipc::SettingKind::text)} == "text");
    }
}

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

#include <cstdint>
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
    sent.decoder = "decode_native";
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
        const std::vector<mp::ipc::Setting> sent{
            {"path", "bitexact", "what may happen to the samples"},
            {"device", "", "or empty for the default"}};
        mp::ipc::Writer w;
        write(w, sent);
        mp::ipc::Reader r{w.bytes()};
        std::vector<mp::ipc::Setting> got;
        REQUIRE(read(r, got));
        CHECK(r.complete());
        REQUIRE(got.size() == sent.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].key == sent[i].key);
            CHECK(got[i].value == sent[i].value);
            CHECK(got[i].description == sent[i].description);
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

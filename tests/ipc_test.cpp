// SPDX-License-Identifier: GPL-3.0-or-later
//
// The door, with a real pipe on the other side of it.
//
// **The test this file exists for is the last one.** The reason the shell is a
// separate process is that killing it must not be audible, and a claim like
// that is worth nothing until something kills one. So: three shells attach to a
// running engine, subscribe to events, and are cut off mid-track without a
// goodbye. The engine plays on, the underrun count stays at zero, and the next
// shell to knock is answered.
//
// Everything else here is about the other half of that: a shell is another
// process, possibly hostile, and the engine has to survive whatever it sends.

#include "fake_host.hpp"

#include "mediaperch/ipc_client.hpp"
#include "mediaperch/ipc_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using mp::test::Host;
using mp::test::pattern;
using mp::test::wait_for;
using mp::test::wait_for_state;

/// A name nothing else is using. Two test runs at once is a normal thing on a
/// build machine, and a fixed name would make them each other's problem -- so
/// the address of a static in this process stands in for a process id, and a
/// counter separates the cases within one run.
std::string unique_pipe(const char* tag)
{
    static std::atomic<int> counter{0};
    return std::string{"\\\\.\\pipe\\mediaperch-test-"} + tag + "-" +
           std::to_string(static_cast<unsigned long long>(
               reinterpret_cast<std::uintptr_t>(&counter))) +
           "-" + std::to_string(counter.fetch_add(1));
}

/// The engine and its door, set up the way `mediaperchd` sets them up.
struct Engine {
    explicit Engine(const char* tag)
        : name(unique_pipe(tag)), player(host), server(player, log, name)
    {
        host.add("long", pattern(64 * 4 * 4000, 1));
        std::string why;
        INFO(why);
        REQUIRE(server.start(why));
        player.start();
    }
    ~Engine()
    {
        server.stop();
        player.shutdown();
    }

    Host host;
    mp::LogRing log;
    std::string name;
    mp::Player player;
    mp::win::IpcServer server;
};

/// Connects and asks one question.
bool ask(const std::string& pipe, mp::ipc::Kind kind, mp::ipc::Header& reply,
         std::vector<std::uint8_t>& body)
{
    mp::win::IpcClient client;
    std::string why;
    if (!client.connect(pipe, 2000, why)) {
        return false;
    }
    return client.call(kind, reply, body, why);
}

} // namespace

TEST_CASE("a shell can ask the engine what it is doing", "[ipc][server]")
{
    Engine engine{"status"};

    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    REQUIRE(ask(engine.name, mp::ipc::Kind::status, reply, body));
    REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));

    mp::ipc::Reader r{body.data(), body.size()};
    mp::ipc::Status status;
    REQUIRE(read(r, status));
    CHECK(status.state == mp::ipc::State::stopped);
    CHECK(status.count == 0);
}

TEST_CASE("a shell can drive the transport", "[ipc][server]")
{
    Engine engine{"drive"};
    mp::win::IpcClient client;
    std::string why;
    REQUIRE(client.connect(engine.name, 2000, why));

    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    mp::ipc::Writer w;
    mp::ipc::write_strings(w, {"long"});
    REQUIRE(client.call(mp::ipc::Kind::play, w, reply, body, why));
    REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::ok));
    REQUIRE(wait_for_state(engine.player, mp::ipc::State::playing));

    REQUIRE(client.call(mp::ipc::Kind::pause, reply, body, why));
    CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::ok));
    CHECK(engine.player.status().state == mp::ipc::State::paused);

    REQUIRE(client.call(mp::ipc::Kind::resume, reply, body, why));
    CHECK(engine.player.status().state == mp::ipc::State::playing);

    // A playlist, and the settings tree, both come back as lists.
    REQUIRE(client.call(mp::ipc::Kind::playlist, reply, body, why));
    REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::playlist_reply));
    mp::ipc::Reader list{body.data(), body.size()};
    std::vector<std::string> files;
    REQUIRE(mp::ipc::read_strings(list, files));
    REQUIRE(files.size() == 1);
    CHECK(files.front() == "long");

    REQUIRE(client.call(mp::ipc::Kind::settings, reply, body, why));
    REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::settings_reply));
    mp::ipc::Reader rows{body.data(), body.size()};
    std::vector<mp::ipc::Setting> settings;
    REQUIRE(read(rows, settings));
    CHECK(settings.size() > 4);
}

TEST_CASE("a setting a shell cannot have is refused with a reason", "[ipc][server]")
{
    Engine engine{"refuse"};
    mp::win::IpcClient client;
    std::string why;
    REQUIRE(client.connect(engine.name, 2000, why));

    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    mp::ipc::Writer w;
    w.str("path");
    w.str("sideways");
    REQUIRE(client.call(mp::ipc::Kind::setting_set, w, reply, body, why));
    REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::error));
    // In the engine's own words, not a code the shell needs a table for.
    CHECK(mp::win::error_text(reply, body).find("bitexact") != std::string::npos);
}

TEST_CASE("several shells can be attached at once", "[ipc][server]")
{
    Engine engine{"many"};
    std::vector<std::unique_ptr<mp::win::IpcClient>> shells;
    for (int i = 0; i < 6; ++i) {
        auto shell = std::make_unique<mp::win::IpcClient>();
        std::string why;
        INFO(why);
        REQUIRE(shell->connect(engine.name, 2000, why));
        shells.push_back(std::move(shell));
    }
    REQUIRE(wait_for([&] { return engine.server.clients() == shells.size(); }));

    for (auto& shell : shells) {
        mp::ipc::Header reply{};
        std::vector<std::uint8_t> body;
        std::string why;
        REQUIRE(shell->call(mp::ipc::Kind::status, reply, body, why));
        CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));
    }
}

TEST_CASE("a shell that sends nonsense is disconnected, not obeyed", "[ipc][server]")
{
    Engine engine{"nonsense"};

    SECTION("a kind the engine has never heard of")
    {
        mp::win::IpcClient client;
        std::string why;
        REQUIRE(client.connect(engine.name, 2000, why));
        mp::ipc::Header reply{};
        std::vector<std::uint8_t> body;
        REQUIRE(client.call(static_cast<mp::ipc::Kind>(4321), reply, body, why));
        CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::error));
        // Refused, and the connection stays: a shell from the future asking for
        // something this build does not have is not a broken shell.
        REQUIRE(client.call(mp::ipc::Kind::status, reply, body, why));
        CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));
    }

    SECTION("a payload that does not match its message")
    {
        mp::win::IpcClient client;
        std::string why;
        REQUIRE(client.connect(engine.name, 2000, why));
        mp::ipc::Writer w;
        w.u32(0xFFFFFFFFu); // a string length nothing can back up
        mp::ipc::Header reply{};
        std::vector<std::uint8_t> body;
        REQUIRE(client.call(mp::ipc::Kind::setting_set, w, reply, body, why));
        CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::error));
    }

    // Whatever any of that did, the engine is still answering.
    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    REQUIRE(ask(engine.name, mp::ipc::Kind::status, reply, body));
    CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));
}

TEST_CASE("killing every shell mid-track changes nothing audible", "[ipc][server]")
{
    // The whole reason the shell is a separate process, as a test.
    Engine engine{"kill"};
    {
        mp::win::IpcClient starter;
        std::string why;
        REQUIRE(starter.connect(engine.name, 2000, why));
        mp::ipc::Writer w;
        mp::ipc::write_strings(w, {"long"});
        mp::ipc::Header reply{};
        std::vector<std::uint8_t> body;
        REQUIRE(starter.call(mp::ipc::Kind::play, w, reply, body, why));
    }
    REQUIRE(wait_for_state(engine.player, mp::ipc::State::playing));
    REQUIRE(wait_for([&] { return engine.player.status().position > 1000; }));

    // Three shells attach and subscribe, so the engine is actively writing to
    // all of them when they go.
    std::vector<std::unique_ptr<mp::win::IpcClient>> shells;
    for (int i = 0; i < 3; ++i) {
        auto shell = std::make_unique<mp::win::IpcClient>();
        std::string why;
        REQUIRE(shell->connect(engine.name, 2000, why));
        mp::ipc::Header reply{};
        std::vector<std::uint8_t> body;
        REQUIRE(shell->call(mp::ipc::Kind::subscribe, reply, body, why));
        REQUIRE(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::ok));
        shells.push_back(std::move(shell));
    }
    // Long enough for at least one event to be on its way to each of them.
    std::this_thread::sleep_for(std::chrono::milliseconds{400});

    const auto before = engine.player.status();
    REQUIRE(before.state == mp::ipc::State::playing);

    // No goodbye: the handles are dropped, which is what a killed process looks
    // like from this side.
    shells.clear();

    REQUIRE(wait_for([&] { return engine.player.status().position > before.position; }));
    const auto after = engine.player.status();
    CHECK(after.state == mp::ipc::State::playing);
    CHECK(after.underruns == 0);

    // And the next shell to knock is answered, which is the part that would
    // fail if the dead ones had wedged the listener.
    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    REQUIRE(ask(engine.name, mp::ipc::Kind::status, reply, body));
    CHECK(reply.kind == static_cast<std::uint16_t>(mp::ipc::Kind::status_reply));
    CHECK(engine.player.status().underruns == 0);
}

TEST_CASE("two engines will not share one name", "[ipc][server]")
{
    // Not a nicety: two engines on one pipe would split a shell's requests
    // between them, and the second would take half the clients silently.
    Engine engine{"single"};
    mp::LogRing log;
    mp::test::Host host;
    mp::Player player{host};
    mp::win::IpcServer second{player, log, engine.name};
    std::string why;
    CHECK_FALSE(second.start(why));
    CHECK(why.find("already listening") != std::string::npos);
}

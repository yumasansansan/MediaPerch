// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine. No window, no toolkit, no user interface of any kind.
//
// **This is the product**, and everything else in the tree is either inside it
// or talking to it. It loads the modules, opens the device, plays what it is
// told to play, and answers a pipe. A shell attaches and detaches; a shell
// crashes; there is no shell at all. None of that reaches the audio, because
// none of it is in the same process.
//
// It prints its log to the console as well as keeping it, because a process
// launched from a terminal that says nothing is a process nobody can debug --
// but nothing it prints is load-bearing, and `mediaperch-cli log` is the real
// answer to "what has it been doing".

#include "mediaperch/engine_host.hpp"
#include "mediaperch/ipc_client.hpp"
#include "mediaperch/ipc_server.hpp"
#include "mediaperch/log.hpp"
#include "mediaperch/platform.hpp"
#include "mediaperch/player.hpp"
#include "mediaperch/win_headers.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_interrupted{false};

BOOL WINAPI on_console_signal(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // Set, not acted on. Stopping a device from inside a console callback
        // is stopping it on a thread Windows is about to take away.
        g_interrupted.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}

void usage()
{
    std::printf(R"(mediaperchd -- the MediaPerch engine

  Runs headless and answers a named pipe. `mediaperch-cli` is the shell that is
  always there; a window is optional and this does not know whether one exists.

usage: mediaperchd [options] [FILE...]

  --pipe NAME       listen somewhere other than \\.\pipe\mediaperch. For a
                    second engine, and for the tests
  --device NAME     part of an endpoint's name. Refuses when it matches two
  --shared          shared mode instead of exclusive
  --path P          bitexact, exactonly, auto or processed
  --dsp SPEC        a stage, repeatable: `name` or `name:key=value,key=value`
  --quiet           keep the log but do not print it
  --help

  Any FILE arguments are played at once, so that an engine started from a
  shortcut is not an engine waiting to be told something obvious.

  Every one of these is also a setting: `mediaperch-cli set path processed`
  changes it while something is playing, and the audio carries on from the frame
  the device stopped on.
)");
}

} // namespace

int main(int argc, char** argv)
{
    const mp::win::ConsoleUtf8 console;
    std::vector<std::string> args = mp::win::command_line_utf8();
    std::vector<char*> utf8_argv;
    if (!args.empty()) {
        utf8_argv.reserve(args.size());
        for (const std::string& arg : args) {
            utf8_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argc = static_cast<int>(utf8_argv.size());
        argv = utf8_argv.data();
    }

    std::string pipe = mp::win::default_pipe_name();
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> settings;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* key) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", key);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--pipe") {
            const char* v = value("--pipe");
            if (v == nullptr) {
                return 1;
            }
            pipe = v;
        } else if (arg == "--device") {
            const char* v = value("--device");
            if (v == nullptr) {
                return 1;
            }
            settings.emplace_back("device", v);
        } else if (arg == "--shared") {
            settings.emplace_back("share", "shared");
        } else if (arg == "--path") {
            const char* v = value("--path");
            if (v == nullptr) {
                return 1;
            }
            settings.emplace_back("path", v);
        } else if (arg == "--dsp") {
            const char* v = value("--dsp");
            if (v == nullptr) {
                return 1;
            }
            // Repeatable, and the order is the order they run in, so they are
            // joined rather than replaced.
            if (!settings.empty() && settings.back().first == "dsp") {
                settings.back().second += std::string{","} + v;
            } else {
                settings.emplace_back("dsp", v);
            }
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 1;
        } else {
            std::error_code ec;
            const auto full = std::filesystem::absolute(arg, ec);
            files.push_back(ec ? arg : full.string());
        }
    }

    const mp::win::ComApartment com;
    if (!com.ok()) {
        std::fprintf(stderr, "could not initialise COM\n");
        return 1;
    }

    mp::LogRing log;
    if (!quiet) {
        // Printed as well as kept. Nothing depends on it.
        (void)log.listen([](const std::string& line) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        });
    }

    mp::win::ModuleRegistry registry;
    registry.scan(mp::win::module_directory());
    if (registry.sink() == nullptr) {
        std::fprintf(stderr, "no sink module beside the executable\n");
        return 1;
    }
    log.add("modules loaded from " + mp::win::module_directory().string());

    mp::win::EngineHost host{registry, log};
    mp::Player player{host};
    for (const auto& [key, value] : settings) {
        std::string why;
        if (!player.set(key, value, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
    }
    player.start();

    mp::win::IpcServer server{player, log, pipe};
    std::string why;
    if (!server.start(why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    log.add("listening on " + pipe);

    SetConsoleCtrlHandler(&on_console_signal, TRUE);
    if (!files.empty()) {
        player.play(files);
    }

    while (!g_interrupted.load(std::memory_order_acquire) && !server.quit_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    log.add("stopping");
    // In this order: the door first, so nothing new arrives while the engine is
    // being taken apart, and the player second, because it is what a shell was
    // asking about.
    server.stop();
    player.shutdown();
    return 0;
}

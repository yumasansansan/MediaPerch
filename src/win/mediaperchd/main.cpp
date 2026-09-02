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
// The one concession to a person with nothing installed is a notification icon
// of its own -- play, pause, next, stop, and a Settings item that says so when
// there is no shell to open. That is a fallback, not a user interface, and it
// lives on the thread that would otherwise be asleep.
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
#include "mediaperch/settings.hpp"
#include "mediaperch/tray.hpp"
#include "mediaperch/win_headers.hpp"

#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

/// `%APPDATA%\MediaPerch\settings.ini`, which is where a Windows program's
/// settings go and where a settings program would look for them.
std::filesystem::path default_config()
{
    wchar_t* roaming = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) &&
        roaming != nullptr) {
        base = roaming;
    }
    CoTaskMemFree(roaming);
    if (base.empty()) {
        return {};
    }
    return base / "MediaPerch" / "settings.ini";
}

bool read_file(const std::filesystem::path& path, std::string& out)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

bool write_file(const std::filesystem::path& path, const std::string& text,
                std::string& why)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        why = "could not open " + path.string() + " to write";
        return false;
    }
    file << text;
    if (!file) {
        why = "could not write " + path.string();
        return false;
    }
    return true;
}

void usage()
{
    std::printf(R"(mediaperchd -- the MediaPerch engine

  Runs headless and answers a named pipe. `mediaperch-cli` is the shell that is
  always there; a window is optional and this does not know whether one exists.

usage: mediaperchd [options] [FILE...]

  --config PATH     the settings file. Default is
                    %%APPDATA%%\MediaPerch\settings.ini, read if it is there.
                    A line it cannot read is complained about in the log rather
                    than refused. `mediaperch-cli save` writes it back
  --no-config       do not read one, and refuse to write one
  --pipe NAME       listen somewhere other than \\.\pipe\mediaperch. For a
                    second engine, and for the tests
  --device NAME     part of an endpoint's name. Refuses when it matches two
  --shared          shared mode instead of exclusive
  --path P          bitexact, exactonly, auto or processed
  --dsp SPEC        a stage, repeatable: `name` or `name:key=value,key=value`
  --no-tray         no notification icon. What a service wants
  --quiet           keep the log but do not print it
  --help

  Options are applied after the settings file, so a flag beats the file for this
  run without changing it.

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

    std::string pipe;
    std::filesystem::path config = default_config();
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> overrides;
    bool quiet = false;
    bool tray = true;

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
        if (arg == "--config") {
            const char* v = value("--config");
            if (v == nullptr) {
                return 1;
            }
            config = v;
        } else if (arg == "--no-config") {
            config.clear();
        } else if (arg == "--pipe") {
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
            overrides.emplace_back("device", v);
        } else if (arg == "--shared") {
            overrides.emplace_back("share", "shared");
        } else if (arg == "--path") {
            const char* v = value("--path");
            if (v == nullptr) {
                return 1;
            }
            overrides.emplace_back("path", v);
        } else if (arg == "--dsp") {
            const char* v = value("--dsp");
            if (v == nullptr) {
                return 1;
            }
            // Repeatable, and the order is the order they run in, so they are
            // joined rather than replaced.
            if (!overrides.empty() && overrides.back().first == "dsp") {
                overrides.back().second += std::string{","} + v;
            } else {
                overrides.emplace_back("dsp", v);
            }
        } else if (arg == "--no-tray") {
            tray = false;
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

    // The settings file first, so that a flag on the command line beats it.
    mp::Settings settings;
    if (!config.empty()) {
        std::string text;
        if (read_file(config, text)) {
            auto file = mp::read_settings(text, config.filename().string());
            settings = std::move(file.settings);
            for (const std::string& complaint : file.complaints) {
                log.add(complaint);
            }
            log.add("settings from " + config.string());
        } else {
            log.add("no settings file at " + config.string() + ", so the defaults it is");
        }
    }
    if (!pipe.empty()) {
        settings.pipe = pipe;
    }
    if (settings.pipe.empty()) {
        settings.pipe = mp::win::default_pipe_name();
    }

    mp::win::ModuleRegistry registry;
    const std::filesystem::path modules = settings.modules.empty()
                                              ? mp::win::module_directory()
                                              : std::filesystem::path{settings.modules};
    registry.scan(modules, settings.allow);
    if (registry.sink() == nullptr) {
        std::fprintf(stderr, "no sink module in %s\n", modules.string().c_str());
        return 1;
    }
    log.add("modules loaded from " + modules.string());
    if (!settings.allow.empty()) {
        log.add("an allow-list is in force, so only what it names was loaded");
    }

    mp::win::EngineHost host{registry, log};
    host.prefer(settings.decoders);
    mp::Player player{host};

    // The file, then the flags. A setting neither names keeps its default, and
    // a setting the file got wrong is named with the line it was on.
    for (const mp::PlayerSetting& setting : settings.player) {
        std::string why;
        if (!player.set(setting.key, setting.value, why)) {
            log.add(config.filename().string() + ":" + std::to_string(setting.line) + ": " +
                    why);
        }
    }
    for (const auto& [key, value] : overrides) {
        std::string why;
        if (!player.set(key, value, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
    }
    player.start();

    mp::win::IpcServer server{player, log, settings.pipe};
    if (!config.empty()) {
        // What `mediaperch-cli save` does. The engine rows are what this run was
        // told; the player rows come from the player, which is the only thing
        // that knows what they are now.
        server.on_save([&](std::string& trouble) {
            mp::Settings out = settings;
            out.player.clear();
            for (const mp::ipc::Setting& row : player.settings()) {
                out.player.push_back(mp::PlayerSetting{row.key, row.value, 0});
            }
            if (!write_file(config, mp::write_settings(out), trouble)) {
                return false;
            }
            log.add("settings written to " + config.string());
            return true;
        });
    }
    std::string why;
    if (!server.start(why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    log.add("listening on " + settings.pipe);

    SetConsoleCtrlHandler(&on_console_signal, TRUE);
    if (!files.empty()) {
        player.play(files);
    }

    const auto keep_going = [&] {
        return !g_interrupted.load(std::memory_order_acquire) && !server.quit_requested();
    };

    mp::win::Tray icon{player, log};
    bool have_tray = false;
    if (tray) {
        std::string trouble;
        have_tray = icon.show(trouble);
        if (!have_tray) {
            // Not an error. An engine does not need a desktop and never did.
            log.add(trouble);
        }
    }
    if (have_tray) {
        icon.run(keep_going);
    } else {
        while (keep_going()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    log.add("stopping");
    icon.hide();
    // In this order: the door first, so nothing new arrives while the engine is
    // being taken apart, and the player second, because it is what a shell was
    // asking about.
    server.stop();
    player.shutdown();
    return 0;
}

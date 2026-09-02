// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shell that is always there.
//
// No toolkit, no window, nothing that costs the machine anything when it is not
// running. It connects, says one thing, prints the answer and exits -- except
// `watch`, which is what a transport bar looks like when it has no pixels.
//
// **Everything it can do, any shell can do**, because it goes through the same
// pipe and the same versioned messages as a window would. If something is
// awkward here it will be awkward there, which makes this the cheapest place to
// find that out.

#include "mediaperch/ipc_client.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/platform.hpp"
#include "mediaperch/protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::printf(R"(mediaperch-cli -- the shell that is always there

usage: mediaperch-cli [--pipe NAME] COMMAND [arguments]

  status            what is playing, in one screen
  play FILE...      replace the playlist and start
  add FILE...       append, without disturbing what is playing
  clear             empty the playlist and stop
  pause | resume | stop
  next | prev
  seek [+|-]SECONDS absolute, or relative when it is signed
  playlist          every track, with an arrow at the current one
  settings          every setting, its value and what it means
  set KEY VALUE     change one. `set path processed` switches paths where it
                    stands: the device stops and starts, and the audio carries
                    on from the frame it stopped on
  save              write the settings file, so what you have set survives a
                    restart. Every key under [player] in it is a `set` key
  log [N]           the last N lines the engine wrote. All of them by default
  watch             follow the engine until Ctrl-C
  quit              ask the engine to stop

  --pipe NAME       an engine listening somewhere other than the default
)");
}

std::string seconds_text(std::uint64_t frames, std::uint32_t rate)
{
    if (rate == 0) {
        return "--:--";
    }
    const auto total = static_cast<unsigned>(frames / rate);
    char out[32];
    std::snprintf(out, sizeof(out), "%u:%02u", total / 60, total % 60);
    return out;
}

void print_status(const mp::ipc::Status& s)
{
    std::printf("state      %s\n", mp::ipc::state_name(s.state));
    if (!s.track.empty()) {
        std::printf("track      %u of %u  %s\n", s.index + 1, s.count, s.track.c_str());
        std::printf("position   %s", seconds_text(s.position, s.source.sample_rate).c_str());
        if (s.length != 0) {
            std::printf(" / %s", seconds_text(s.length, s.source.sample_rate).c_str());
        }
        std::printf("  (%llu frames)\n", static_cast<unsigned long long>(s.position));
    }
    if (!s.decoder.empty()) {
        std::printf("decoder    %s\n", s.decoder.c_str());
    }
    if (!s.device.empty()) {
        std::printf("device     %s\n", s.device.c_str());
        std::printf("format     %s\n", mp::describe(s.wire).c_str());
        std::printf("source     %s\n", mp::describe(s.source).c_str());
        // The claim the whole program is about, in one line.
        std::printf("path       %s\n",
                    s.processed ? "PROCESSED -- the samples are changed"
                    : s.fidelity == static_cast<std::uint32_t>(mp::Fidelity::exact)
                        ? "passthrough, memcpy"
                        : "passthrough, container repack");
    }
    if (s.frames_rendered != 0) {
        std::printf("played     %llu frames, %llu underruns\n",
                    static_cast<unsigned long long>(s.frames_rendered),
                    static_cast<unsigned long long>(s.underruns));
    }
    if (!s.error.empty()) {
        std::printf("last error %s\n", s.error.c_str());
    }
}

/// A path the engine can open. It has its own working directory, and a shell
/// that sent `track.flac` would be asking it to guess.
std::string absolute(const std::string& path)
{
    std::error_code ec;
    const auto full = std::filesystem::absolute(path, ec);
    return ec ? path : full.string();
}

int fail(const std::string& why)
{
    std::fprintf(stderr, "%s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    const mp::win::ConsoleUtf8 console;
    std::vector<std::string> args = mp::win::command_line_utf8();
    if (args.empty()) {
        for (int i = 0; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
    }

    std::string pipe = mp::win::default_pipe_name();
    std::size_t at = 1;
    while (at + 1 < args.size() && args[at] == "--pipe") {
        pipe = args[at + 1];
        at += 2;
    }
    if (at >= args.size() || args[at] == "--help" || args[at] == "-h") {
        usage();
        return at >= args.size() ? 1 : 0;
    }
    const std::string command = args[at];
    const std::vector<std::string> rest{args.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                                        args.end()};

    mp::win::IpcClient client;
    std::string why;
    if (!client.connect(pipe, 2000, why)) {
        return fail(why);
    }

    mp::ipc::Header reply{};
    std::vector<std::uint8_t> body;
    mp::ipc::Writer w;

    const auto simple = [&](mp::ipc::Kind kind) {
        if (!client.call(kind, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    };

    if (command == "status") {
        if (!client.call(mp::ipc::Kind::status, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        mp::ipc::Status status;
        if (!read(r, status)) {
            return fail("the engine sent a status this build cannot read");
        }
        print_status(status);
        return 0;
    }
    if (command == "play" || command == "add") {
        if (rest.empty()) {
            return fail(command + " needs at least one file");
        }
        std::vector<std::string> files;
        files.reserve(rest.size());
        for (const std::string& path : rest) {
            files.push_back(absolute(path));
        }
        mp::ipc::write_strings(w, files);
        if (!client.call(command == "play" ? mp::ipc::Kind::play : mp::ipc::Kind::enqueue,
                         w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    }
    if (command == "pause") {
        return simple(mp::ipc::Kind::pause);
    }
    if (command == "resume") {
        return simple(mp::ipc::Kind::resume);
    }
    if (command == "stop") {
        return simple(mp::ipc::Kind::stop);
    }
    if (command == "clear") {
        return simple(mp::ipc::Kind::clear);
    }
    if (command == "next") {
        return simple(mp::ipc::Kind::next);
    }
    if (command == "prev" || command == "previous") {
        return simple(mp::ipc::Kind::previous);
    }
    if (command == "quit") {
        return simple(mp::ipc::Kind::quit);
    }
    if (command == "save") {
        return simple(mp::ipc::Kind::save);
    }
    if (command == "seek") {
        if (rest.empty()) {
            return fail("seek needs a number of seconds");
        }
        const std::string& text = rest.front();
        const bool relative = text[0] == '+' || text[0] == '-';
        char* end = nullptr;
        const double seconds = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) {
            return fail("`" + text + "` is not a number of seconds");
        }
        // The engine counts in frames, so the conversion needs a rate, and the
        // only honest source of one is what is playing now.
        if (!client.call(mp::ipc::Kind::status, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        mp::ipc::Status status;
        if (!read(r, status) || status.source.sample_rate == 0) {
            return fail("nothing is playing, so there is nothing to seek in");
        }
        w.u8(relative ? 1u : 0u);
        w.i64(static_cast<std::int64_t>(seconds * status.source.sample_rate));
        if (!client.call(mp::ipc::Kind::seek, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    }
    if (command == "playlist") {
        if (!client.call(mp::ipc::Kind::playlist, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<std::string> files;
        if (!mp::ipc::read_strings(r, files)) {
            return fail("the engine sent a playlist this build cannot read");
        }
        const std::uint32_t current = r.u32();
        if (files.empty()) {
            std::printf("the playlist is empty\n");
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::printf("%s %3zu  %s\n", i == current ? "->" : "  ", i + 1,
                        files[i].c_str());
        }
        return 0;
    }
    if (command == "settings") {
        if (!client.call(mp::ipc::Kind::settings, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<mp::ipc::Setting> settings;
        if (!read(r, settings)) {
            return fail("the engine sent settings this build cannot read");
        }
        for (const mp::ipc::Setting& s : settings) {
            std::printf("%-16s %-24s %s\n", s.key.c_str(), s.value.c_str(),
                        s.description.c_str());
        }
        return 0;
    }
    if (command == "set") {
        if (rest.size() < 2) {
            return fail("set needs a key and a value");
        }
        w.str(rest[0]);
        w.str(rest[1]);
        if (!client.call(mp::ipc::Kind::setting_set, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    }
    if (command == "log") {
        w.u32(rest.empty() ? 0u
                           : static_cast<std::uint32_t>(std::strtoul(rest[0].c_str(),
                                                                     nullptr, 10)));
        if (!client.call(mp::ipc::Kind::log, w, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<std::string> lines;
        if (!mp::ipc::read_strings(r, lines)) {
            return fail("the engine sent a log this build cannot read");
        }
        for (const std::string& line : lines) {
            std::printf("%s\n", line.c_str());
        }
        return 0;
    }
    if (command == "watch") {
        if (!client.call(mp::ipc::Kind::subscribe, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        std::printf("watching %s -- Ctrl-C to stop\n", pipe.c_str());
        std::string last;
        while (client.receive(reply, body)) {
            mp::ipc::Reader r{body.data(), body.size()};
            switch (static_cast<mp::ipc::Kind>(reply.kind)) {
            case mp::ipc::Kind::event_state: {
                mp::ipc::Status status;
                if (!read(r, status)) {
                    break;
                }
                // One line, rewritten in place: a transport bar with no pixels.
                char line[512];
                std::snprintf(line, sizeof(line), "%-8s %s %s",
                              mp::ipc::state_name(status.state),
                              seconds_text(status.position, status.source.sample_rate)
                                  .c_str(),
                              status.track.c_str());
                if (line != last) {
                    std::printf("\r%-118s", line);
                    std::fflush(stdout);
                    last = line;
                }
                break;
            }
            case mp::ipc::Kind::event_log: {
                const std::string text = r.str();
                if (r.ok()) {
                    std::printf("\r%-118s\n", text.c_str());
                    std::fflush(stdout);
                    last.clear();
                }
                break;
            }
            default:
                break;
            }
        }
        std::printf("\nthe engine went away\n");
        return 0;
    }

    usage();
    return fail("there is no command called `" + command + "`");
}

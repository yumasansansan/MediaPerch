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
#include "mediaperch/win_headers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// The help, and after it what kind of value the row takes when that is not
/// simply text: `[choice: a, b, c]`, `[integer, unit=ms]`, `[toggle]`. The same
/// field a shell draws a control from, printed so that a person can see what
/// the shell will see.
std::string kind_text(const mp::ipc::Setting& row)
{
    std::string out = row.description;
    if (row.read_only || row.kind == mp::ipc::SettingKind::text) {
        return out;
    }
    out += "  [";
    out += mp::ipc::setting_kind_name(row.kind);
    if (!row.choices.empty()) {
        out += ": ";
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            out += (i == 0 ? "" : ", ") + row.choices[i];
        }
    }
    if (!row.hints.empty()) {
        out += ", " + row.hints;
    }
    if (!row.when.empty()) {
        out += ", when " + row.when;
    }
    out += "]";
    return out;
}

void usage()
{
    std::printf(R"(mediaperch-cli -- the shell that is always there

usage: mediaperch-cli [--pipe NAME] COMMAND [arguments]

  status            what is playing, in one screen
  play FILE...      replace the playlist and start
  add FILE...       append, without disturbing what is playing
  goto N            play track N of the playlist, as `playlist` numbers them
  move A B          move track A to position B; only tracks the engine has not
                    reached yet can move while something plays
  clear             empty the playlist and stop
  pause | resume | stop
  next | prev
  seek [+|-]SECONDS absolute, or relative when it is signed
  display WHAT      which display the picture is on: `probe` to let the engine
                    work it out, or hdr|sdr with the white level and the peak
                    in nits -- `display hdr 480 1000`. A shell sends this when
                    its window crosses a monitor; here it is mostly a way to
                    see what a display it is not on would do
  calibrate FILE... measure what this machine needs to play these files and keep
                    the answer. **Takes the engine over** and takes minutes: a
                    calibration cannot run faster than the material. Progress is
                    log lines, so `log` or a subscribed shell shows it
  profile           what the last calibration concluded, as the text of the file
  graph             the shape the engine is in, as nodes and what connects them.
                    A shell draws this as a canvas; here it is one line a node
  node ID [KEY VAL] one node's own settings, or change one of them. `graph`
                    gives the ids
  modules           every module that is loaded, with its kind and priority
  surface           the composition surface the picture is drawn into, duplicated
                    into this process. Mostly a way to see whether there is one:
                    a shell puts it in a visual, and this one prints it and
                    closes it again
  engine [KEY VAL]  the [engine] half of the settings file: where it listens,
                    where modules are, which may load, which readers to prefer,
                    where the profile is. **These take effect at the next
                    start**, which is why they are not `set` keys
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
        // **Into this track, against this track's length.** `position` is the
        // queue's coordinate and is what a seek speaks; printing it against a
        // track's length is how this said `0:14 / 0:01`.
        std::printf("position   %s", seconds_text(s.item_position, s.clock_rate).c_str());
        if (s.length != 0) {
            std::printf(" / %s", seconds_text(s.length, s.clock_rate).c_str());
        }
        std::printf("  (%llu frames into the queue)\n",
                    static_cast<unsigned long long>(s.position));
    }
    if (!s.decoder.empty()) {
        std::printf("decoder    %s\n", s.decoder.c_str());
    }
    if (s.device.empty() && s.state != mp::ipc::State::stopped && !s.track.empty()) {
        // No device because there is no audio: the picture keeps its own
        // clock, and the position above is in its milliseconds.
        std::printf("audio      none in this track; the picture is on the video engine's "
                    "own clock\n");
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
    if (command == "move") {
        // One-based, as `playlist` prints them.
        if (rest.size() < 2 || std::atoi(rest[0].c_str()) < 1 || std::atoi(rest[1].c_str()) < 1) {
            return fail("move needs two track numbers, as `playlist` shows them: from and to");
        }
        w.u32(static_cast<std::uint32_t>(std::atoi(rest[0].c_str()) - 1));
        w.u32(static_cast<std::uint32_t>(std::atoi(rest[1].c_str()) - 1));
        if (!client.call(mp::ipc::Kind::move_entry, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    }
    if (command == "goto") {
        // One-based, as `playlist` prints them.
        if (rest.empty() || std::atoi(rest[0].c_str()) < 1) {
            return fail("goto needs the track's number, as `playlist` shows it");
        }
        w.u32(static_cast<std::uint32_t>(std::atoi(rest[0].c_str()) - 1));
        if (!client.call(mp::ipc::Kind::play_at, w, reply, body, why)) {
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
        // The engine counts in frames of its clock -- the source's rate while
        // audio plays, the picture's own clock's while a picture plays alone --
        // and the only honest source of the rate is what is playing now.
        if (!client.call(mp::ipc::Kind::status, reply, body, why)) {
            return fail(why);
        }
        mp::ipc::Reader r{body.data(), body.size()};
        mp::ipc::Status status;
        if (!read(r, status) || status.clock_rate == 0 ||
            status.state == mp::ipc::State::stopped) {
            return fail("nothing is playing, so there is nothing to seek in");
        }
        w.u8(relative ? 1u : 0u);
        w.i64(static_cast<std::int64_t>(seconds * status.clock_rate));
        if (!client.call(mp::ipc::Kind::seek, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        return 0;
    }
    if (command == "calibrate") {
        if (rest.empty()) {
            return fail("calibrate needs files to measure");
        }
        mp::ipc::Calibration asked;
        asked.files = rest;
        write(w, asked);
        if (!client.call(mp::ipc::Kind::calibrate, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
            return fail(mp::win::error_text(reply, body));
        }
        std::printf("measuring %zu file%s; watch `mediaperch-cli log` for progress\n",
                    rest.size(), rest.size() == 1 ? "" : "s");
        return 0;
    }
    if (command == "profile") {
        if (!client.call(mp::ipc::Kind::profile, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::profile_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        const std::string text = r.str();
        if (!r.complete()) {
            return fail("the engine sent a profile this build cannot read");
        }
        if (text.empty()) {
            std::printf("nothing has been measured on this machine yet\n");
            return 0;
        }
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (command == "graph") {
        if (!client.call(mp::ipc::Kind::graph, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::graph_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        mp::ipc::Graph graph;
        if (!read(r, graph)) {
            return fail("the engine sent a shape this build cannot read");
        }
        static const char* const kinds[] = {"source",  "convert",     "dsp",
                                            "sink",    "video",       "video stage",
                                            "presenter"};
        for (const mp::ipc::Node& node : graph.nodes) {
            const char* kind = node.kind < std::size(kinds) ? kinds[node.kind] : "?";
            std::printf("%-12s %-12s %s%s%s\n", node.id.c_str(), kind,
                        node.name.c_str(),
                        node.module.empty() ? "" : "  [",
                        node.module.empty() ? "" : (node.module + "]").c_str());
        }
        // The edges after the nodes, because what a person reads first is what
        // is in the run and not how it is joined.
        for (const mp::ipc::Edge& edge : graph.edges) {
            std::printf("             %s -> %s\n", edge.from.c_str(), edge.to.c_str());
        }
        return 0;
    }
    if (command == "node") {
        if (rest.empty()) {
            return fail("node needs an id; `graph` lists them");
        }
        if (rest.size() >= 3) {
            w.str(rest[0]);
            w.str(rest[1]);
            w.str(rest[2]);
            if (!client.call(mp::ipc::Kind::node_setting_set, w, reply, body, why)) {
                return fail(why);
            }
            if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
                return fail(mp::win::error_text(reply, body));
            }
            return 0;
        }
        w.str(rest[0]);
        if (!client.call(mp::ipc::Kind::node_settings, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::node_settings_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<mp::ipc::Setting> rows;
        if (!read(r, rows)) {
            return fail("the engine sent settings this build cannot read");
        }
        if (rows.empty()) {
            std::printf("`%s` has no settings of its own\n", rest[0].c_str());
            return 0;
        }
        for (const mp::ipc::Setting& row : rows) {
            std::printf("%-16s %-24s %s\n", row.key.c_str(), row.value.c_str(),
                        kind_text(row).c_str());
        }
        return 0;
    }
    if (command == "surface") {
        w.u32(GetCurrentProcessId());
        if (!client.call(mp::ipc::Kind::surface, w, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::surface_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        const std::uint64_t handle = r.u64();
        const std::uint64_t generation = r.u64();
        if (!r.complete()) {
            return fail("the engine sent a surface this build cannot read");
        }
        if (handle == 0) {
            std::printf("no surface: nothing is showing a picture, or the presenter "
                        "draws into a window\n");
            return 0;
        }
        std::printf("surface    0x%llx, duplicated into this process\n",
                    static_cast<unsigned long long>(handle));
        // Which picture, so two of them can be told apart when the handle
        // cannot do it -- a duplicate is a new number on every call.
        std::printf("picture    #%llu of this run\n",
                    static_cast<unsigned long long>(generation));
        // **Ours now, so ours to close.** A handle a shell keeps is a
        // composition surface the engine cannot let go of.
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)));
        return 0;
    }
    if (command == "engine") {
        if (rest.size() >= 2) {
            w.str(rest[0]);
            w.str(rest[1]);
            if (!client.call(mp::ipc::Kind::engine_setting_set, w, reply, body, why)) {
                return fail(why);
            }
            if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::ok) {
                return fail(mp::win::error_text(reply, body));
            }
            std::printf("set, and it takes effect at the next start. "
                        "`save` writes it down\n");
            return 0;
        }
        if (!client.call(mp::ipc::Kind::engine_settings, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) !=
            mp::ipc::Kind::engine_settings_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<mp::ipc::Setting> rows;
        if (!read(r, rows)) {
            return fail("the engine sent settings this build cannot read");
        }
        for (const mp::ipc::Setting& row : rows) {
            std::printf("%-10s %-28s %s\n", row.key.c_str(), row.value.c_str(),
                        kind_text(row).c_str());
        }
        return 0;
    }
    if (command == "modules") {
        if (!client.call(mp::ipc::Kind::modules, reply, body, why)) {
            return fail(why);
        }
        if (static_cast<mp::ipc::Kind>(reply.kind) != mp::ipc::Kind::modules_reply) {
            return fail(mp::win::error_text(reply, body));
        }
        mp::ipc::Reader r{body.data(), body.size()};
        std::vector<mp::ipc::ModuleRow> rows;
        if (!read(r, rows)) {
            return fail("the engine sent a module list this build cannot read");
        }
        for (const mp::ipc::ModuleRow& row : rows) {
            std::printf("%-18s kind %2u  priority %3u  %s\n", row.id.c_str(), row.kind,
                        row.priority, row.name.c_str());
        }
        return 0;
    }
    if (command == "display") {
        if (rest.empty()) {
            return fail("display needs `probe`, or hdr|sdr with a white level and a peak");
        }
        if (rest.front() == "probe") {
            w.u8(0u); // not known: the engine works it out
            w.u8(0u);
            w.u8(0u);
            w.f64(0.0);
            w.f64(0.0);
        } else {
            const bool hdr = rest.front() == "hdr";
            if (!hdr && rest.front() != "sdr") {
                return fail("`" + rest.front() + "` is not hdr, sdr or probe");
            }
            // **The standards' defaults when nothing is said**, which is what a
            // shell that cannot tell should send: 80 nits is scRGB's reference
            // white and 1000 is BT.2100's reference display.
            double white = 80.0;
            double peak = 1000.0;
            for (std::size_t i = 1; i < rest.size() && i <= 2; ++i) {
                char* end = nullptr;
                const double nits = std::strtod(rest[i].c_str(), &end);
                if (end == rest[i].c_str() || nits <= 0.0) {
                    return fail("`" + rest[i] + "` is not a number of nits");
                }
                (i == 1 ? white : peak) = nits;
            }
            w.u8(1u);
            w.u8(hdr ? 1u : 0u);
            w.u8(0u); // wide: only Windows 11 24H2 can tell, and a person cannot
            w.f64(white);
            w.f64(peak);
        }
        if (!client.call(mp::ipc::Kind::display, w, reply, body, why)) {
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
                        kind_text(s).c_str());
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

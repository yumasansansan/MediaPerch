// SPDX-License-Identifier: GPL-3.0-or-later
//
// mediaperch-probe: milestone 1's whole user interface.
//
// Three commands, and the difference between them is how much they disturb the
// machine. Listing devices opens nothing. The other two take the endpoint, which
// in exclusive mode silences every other application on it -- so each says so
// before it does it.

#include "mediaperch/platform.hpp"

#include "mediaperch/negotiation.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/sine.hpp"
#include "mediaperch/sink.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string command = "devices";
    int device_index = -1; // -1 = the system default
    std::uint32_t rate = 44100;
    std::uint32_t bits = 16;
    std::uint32_t channels = 2;
    double hz = 1000.0;
    double amplitude = 0.5;
    unsigned seconds = 10;
    bool shared = false;
    bool verbose = false;
};

void usage()
{
    std::puts(R"(mediaperch-probe -- milestone 1

  devices     list render endpoints. Opens nothing and disturbs nothing.
  negotiate   offer every candidate format to a device for real.
              TAKES THE ENDPOINT briefly. In exclusive mode that silences
              every other application on it.
  play        play a test tone.
              TAKES THE ENDPOINT for the whole duration.

Options
  --device N        index from `devices`; default is the system default endpoint
  --rate R          default 44100
  --bits 16|24|32   24 means 24 valid bits in a 32-bit container; default 16
  --channels N      default 2
  --hz F            tone frequency, default 1000
  --amplitude A     fraction of full scale, default 0.5 (-6 dBFS).
                    Turn this down for headphones: 0.02 is about -34 dBFS.
  --seconds N       play duration, default 10
  --shared          shared mode instead of exclusive
  --verbose

There is deliberately no "just ask IsFormatSupported" command. In exclusive mode
drivers answer that optimistically and refuse in Initialize, so the sink ABI
offers one answer because there is one answer.)");
}

bool parse(int argc, char** argv, Options& out)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](std::uint32_t& target) {
            if (i + 1 < argc) {
                target = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            }
        };

        if (arg == "-h" || arg == "--help") {
            usage();
            std::exit(0);
        } else if (arg == "devices" || arg == "negotiate" || arg == "play") {
            out.command = arg;
        } else if (arg == "--device") {
            if (i + 1 < argc) {
                out.device_index = std::atoi(argv[++i]);
            }
        } else if (arg == "--rate") {
            value(out.rate);
        } else if (arg == "--bits") {
            value(out.bits);
        } else if (arg == "--channels") {
            value(out.channels);
        } else if (arg == "--hz") {
            if (i + 1 < argc) {
                out.hz = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--amplitude") {
            if (i + 1 < argc) {
                out.amplitude = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--seconds") {
            std::uint32_t s = 0;
            value(s);
            out.seconds = s;
        } else if (arg == "--shared") {
            out.shared = true;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n\n", arg.c_str());
            usage();
            return false;
        }
    }
    return true;
}

const char* result_name(MpResult r)
{
    switch (r) {
    case MP_OK: return "ok";
    case MP_END: return "end";
    case MP_ERR_INVALID: return "invalid";
    case MP_ERR_UNSUPPORTED: return "unsupported by this module";
    case MP_ERR_FORMAT: return "format refused";
    case MP_ERR_IO: return "io";
    case MP_ERR_DEVICE_LOST: return "device lost";
    case MP_ERR_BUSY: return "device in use by another application";
    case MP_ERR_DENIED: return "exclusive mode is disabled for this device";
    case MP_ERR_NO_MEMORY: return "out of memory";
    case MP_TIMEOUT: return "timed out";
    default: return "internal error";
    }
}

mp::Format requested_format(const Options& options)
{
    mp::Format format;
    format.sample_rate = options.rate;
    format.channels = options.channels;
    format.channel_mask = 0;
    format.encoding = mp::Encoding::pcm;

    switch (options.bits) {
    case 16:
        format.sample_type = mp::SampleType::s16;
        break;
    case 24:
        format.sample_type = mp::SampleType::s24_in_32;
        format.valid_bits = 24;
        break;
    case 32:
        format.sample_type = mp::SampleType::s32;
        break;
    default:
        format.sample_type = mp::SampleType::none;
        break;
    }
    return format;
}

/// Loads the WASAPI sink from beside the executable.
std::unique_ptr<mp::win::LoadedModule> load_sink_module()
{
    const auto path = mp::win::module_directory() / "mp_sink_wasapi.dll";
    auto module = mp::win::LoadedModule::load(path);
    if (!module) {
        std::fprintf(stderr, "could not load %s\n", path.string().c_str());
        return nullptr;
    }
    if (module->sink_vtbl() == nullptr) {
        std::fprintf(stderr, "%s is not a sink module\n", path.string().c_str());
        return nullptr;
    }
    return module;
}

int list_devices(const MpSinkVtbl& sink)
{
    MpDeviceInfo info{};
    for (std::uint32_t index = 0;; ++index) {
        info.size = sizeof(info);
        const MpResult r = sink.enumerate(index, &info);
        if (r == MP_END) {
            if (index == 0) {
                std::puts("no active render endpoints");
            }
            return 0;
        }
        if (r != MP_OK) {
            std::fprintf(stderr, "enumerate failed: %s\n", result_name(r));
            return 1;
        }
        std::printf("%2u %s%s\n     %s\n", index,
                    (info.flags & MP_DEVICE_IS_DEFAULT) != 0 ? "* " : "  ", info.name, info.id);
    }
}

/// Resolve --device N into an endpoint id. Empty means the default endpoint.
bool device_id_for(const MpSinkVtbl& sink, int index, std::string& out)
{
    if (index < 0) {
        out.clear();
        return true;
    }
    MpDeviceInfo info{};
    info.size = sizeof(info);
    const MpResult r = sink.enumerate(static_cast<std::uint32_t>(index), &info);
    if (r != MP_OK) {
        std::fprintf(stderr, "no device with index %d\n", index);
        return false;
    }
    out = info.id;
    return true;
}

mp::Sink open_sink(const MpSinkVtbl& vtbl, const std::string& device_id, bool shared)
{
    MpSink* handle = nullptr;
    const MpResult r = vtbl.open(device_id.empty() ? nullptr : device_id.c_str(),
                                 shared ? MP_SHARE_SHARED : MP_SHARE_EXCLUSIVE, &handle);
    if (r != MP_OK) {
        std::fprintf(stderr, "open failed: %s\n", result_name(r));
        return {};
    }
    return mp::Sink{&vtbl, handle};
}

int negotiate(const MpSinkVtbl& vtbl, const Options& options)
{
    const mp::Format source = requested_format(options);
    if (!mp::is_valid(source)) {
        std::fprintf(stderr, "that is not a format: %s\n", mp::describe(source).c_str());
        return 1;
    }

    std::string device_id;
    if (!device_id_for(vtbl, options.device_index, device_id)) {
        return 1;
    }
    mp::Sink sink = open_sink(vtbl, device_id, options.shared);
    if (!sink) {
        return 1;
    }

    std::printf("source     %s\n", mp::describe(source).c_str());
    std::printf("mode       %s\n\n", options.shared ? "shared" : "exclusive");

    const auto candidates = mp::build_candidates(source);
    std::size_t index = 0;
    for (const auto& candidate : candidates) {
        mp::Format accepted{};
        const MpResult r = sink.negotiate(candidate.format, accepted);
        const char* label = candidate.fidelity == mp::Fidelity::exact ? "exact" : "repack";

        std::printf("%zu %-7s %-5s  %-44s ", ++index, label,
                    candidate.channel_mask_added ? "+mask" : "",
                    mp::describe(candidate.format).c_str());

        if (r != MP_OK) {
            std::printf("-> %s\n", result_name(r));
            continue;
        }

        const mp::Fidelity actual = mp::classify(source, accepted);
        if (!mp::is_bit_exact(actual)) {
            // The device said yes and meant something else. Not a success.
            std::printf("-> accepted %s -- NOT bit-exact, refusing\n",
                        mp::describe(accepted).c_str());
            continue;
        }

        std::uint32_t period = 0;
        sink.period_frames(period);
        std::printf("-> ACCEPTED, buffer %u frames (%.2f ms)\n", period,
                    1000.0 * period / accepted.sample_rate);
        return 0;
    }

    std::printf("\nnothing was accepted. %zu candidates offered.\n", candidates.size());
    return 1;
}

int play(const MpSinkVtbl& vtbl, const Options& options)
{
    const mp::Format source_format = requested_format(options);
    if (!mp::is_valid(source_format)) {
        std::fprintf(stderr, "that is not a format: %s\n",
                     mp::describe(source_format).c_str());
        return 1;
    }

    std::string device_id;
    if (!device_id_for(vtbl, options.device_index, device_id)) {
        return 1;
    }
    mp::Sink sink = open_sink(vtbl, device_id, options.shared);
    if (!sink) {
        return 1;
    }

    const auto negotiated = mp::negotiate_best(sink, source_format);
    if (!negotiated.ok) {
        std::fprintf(stderr, "no candidate was accepted (%zu offered): %s\n", negotiated.tried,
                     result_name(negotiated.last_error));
        return 1;
    }

    std::uint32_t period = 0;
    if (sink.period_frames(period) != MP_OK || period == 0) {
        std::fprintf(stderr, "the device did not report a buffer size\n");
        return 1;
    }

    std::printf("device     %s\n", device_id.empty() ? "(default endpoint)" : device_id.c_str());
    std::printf("mode       %s\n", options.shared ? "shared" : "exclusive");
    std::printf("format     %s\n", mp::describe(negotiated.accepted).c_str());
    std::printf("path       %s%s\n",
                negotiated.fidelity == mp::Fidelity::exact
                    ? "passthrough, memcpy"
                    : "passthrough, container repack",
                negotiated.channel_mask_added ? " (extensible form)" : "");
    std::printf("buffer     %u frames (%.2f ms)\n", period,
                1000.0 * period / negotiated.accepted.sample_rate);
    std::printf("tone       %.1f Hz at %.3f of full scale (%.1f dBFS) for %u s\n\n",
                options.hz, options.amplitude,
                options.amplitude > 0.0 ? 20.0 * std::log10(options.amplitude) : -144.0,
                options.seconds);

    mp::SineSource source{source_format, options.hz, options.amplitude};
    mp::win::RenderThreadHooks hooks;
    mp::PassthroughGraph graph{source, sink, negotiated.accepted, period, negotiated.fidelity,
                               &hooks};

    const MpResult started = graph.start();
    if (started != MP_OK) {
        std::fprintf(stderr, "could not start: %s\n", result_name(started));
        return 1;
    }
    // enter() runs on the render thread, which has not necessarily reached it
    // yet. Ask, rather than read a field that has not been written.
    switch (hooks.wait_for_answer(200)) {
    case mp::win::RenderThreadHooks::Realtime::granted:
        std::printf("mmcss      Pro Audio\n\n");
        break;
    case mp::win::RenderThreadHooks::Realtime::refused:
        std::printf("mmcss      REFUSED (error %lu) -- not real-time\n\n",
                    hooks.refusal_code());
        break;
    case mp::win::RenderThreadHooks::Realtime::pending:
        std::printf("mmcss      the render thread has not answered yet\n\n");
        break;
    }

    const auto began = std::chrono::steady_clock::now();
    const auto until = began + std::chrono::seconds{options.seconds};
    auto next_report = began + std::chrono::seconds{5};

    while (std::chrono::steady_clock::now() < until && graph.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_report) {
            const auto stats = graph.stats();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - began).count();
            std::printf("  %4lld s   %llu frames   %llu underruns\n",
                        static_cast<long long>(elapsed),
                        static_cast<unsigned long long>(stats.frames_rendered),
                        static_cast<unsigned long long>(stats.underruns));
            std::fflush(stdout);
            next_report = now + std::chrono::seconds{5};
        }
    }

    graph.stop();

    const auto stats = graph.stats();
    const double seconds = static_cast<double>(stats.frames_rendered) /
                           negotiated.accepted.sample_rate;
    std::printf("\nplayed     %llu frames (%.2f s)\n",
                static_cast<unsigned long long>(stats.frames_rendered), seconds);
    std::printf("underruns  %llu", static_cast<unsigned long long>(stats.underruns));
    if (stats.underruns != 0) {
        std::printf("  (%llu frames of silence)",
                    static_cast<unsigned long long>(stats.silent_frames));
    }
    std::printf("\ntimeouts   %llu\n", static_cast<unsigned long long>(stats.wait_timeouts));

    const MpResult error = graph.error();
    if (error != MP_OK) {
        std::printf("stopped    %s\n", result_name(error));
        return 1;
    }
    return stats.underruns == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse(argc, argv, options)) {
        return 1;
    }
    mp::win::set_log_level(options.verbose ? MP_LOG_DEBUG : MP_LOG_INFO);

    const mp::win::ComApartment com;
    if (!com.ok()) {
        std::fprintf(stderr, "could not initialise COM\n");
        return 1;
    }

    const auto module = load_sink_module();
    if (!module) {
        return 1;
    }
    const MpSinkVtbl& sink = *module->sink_vtbl();

    if (options.command == "devices") {
        return list_devices(sink);
    }
    if (options.command == "negotiate") {
        return negotiate(sink, options);
    }
    if (options.command == "play") {
        return play(sink, options);
    }

    usage();
    return 1;
}

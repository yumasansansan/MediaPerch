// SPDX-License-Identifier: GPL-3.0-or-later
//
// mediaperch-probe: milestone 1's whole user interface.
//
// Three commands, and the difference between them is how much they disturb the
// machine. Listing devices opens nothing. The other two take the endpoint, which
// in exclusive mode silences every other application on it -- so each says so
// before it does it.

#include "mediaperch/platform.hpp"

#include "mediaperch/decoder.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/repack.hpp"
#include "mediaperch/sine.hpp"
#include "mediaperch/sink.hpp"
#include "mediaperch/verify.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string command = "devices";
    std::string file;
    std::string raw;
    std::string capture; // capture endpoint id; empty = find one by name
    int capture_index = -1;
    int device_index = -1; // -1 = the system default
    std::uint32_t rate = 44100;
    std::uint32_t bits = 16;
    std::uint32_t channels = 2;
    double hz = 1000.0;
    double amplitude = 0.5;
    unsigned seconds = 10;
    bool shared = false;
    bool loopback = false;
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
  decode      decode a file and print SHA-256 of the PCM it produced. Touches no
              device. Compare it with a reference decoder to check this one.
  verify      play a file into a loopback and record the other end, then compare
              SHA-256 of what was sent with SHA-256 of what came back.
              TAKES TWO ENDPOINTS -- a render one and a capture one -- and is
              only meaningful when they are the two halves of a virtual cable.

Options
  --file PATH       the file to decode. WAV and FLAC
  --raw PATH        `decode` writes the decoded PCM here. `verify` writes what it
                    sent to PATH.sent and what it recorded to PATH.recorded
  --device N        index from `devices`; default is the system default endpoint
  --capture N       capture endpoint index; default is one named "CABLE Output"
  --rate R          default 44100
  --bits 16|24|32   24 means 24 valid bits in a 32-bit container; default 16
  --channels N      default 2
  --hz F            tone frequency, default 1000
  --amplitude A     fraction of full scale, default 0.5 (-6 dBFS).
                    Turn this down for headphones: 0.02 is about -34 dBFS.
                    This is a property of the generator, not a gain stage: the
                    tone is computed and quantised once, and nothing scales a
                    sample afterwards. It does decide how much of the container
                    the tone exercises, which `play` reports.
  --seconds N       play duration, default 10
  --shared          shared mode instead of exclusive
  --loopback        `verify` also records from a capture endpoint and reports
                    whether the loopback returned the bytes unchanged. Read
                    docs/devices.md before believing a failure here
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
        } else if (arg == "devices" || arg == "negotiate" || arg == "play" ||
                   arg == "verify" || arg == "decode") {
            out.command = arg;
        } else if (arg == "--file") {
            if (i + 1 < argc) {
                out.file = argv[++i];
            }
        } else if (arg == "--raw") {
            if (i + 1 < argc) {
                out.raw = argv[++i];
            }
        } else if (arg == "--capture") {
            if (i + 1 < argc) {
                out.capture_index = std::atoi(argv[++i]);
            }
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
        } else if (arg == "--loopback") {
            out.loopback = true;
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
        std::printf("%2u %s%s%s\n     %s\n", index,
                    (info.flags & MP_DEVICE_IS_DEFAULT) != 0 ? "* " : "  ", info.name,
                    (info.flags & MP_DEVICE_ENDPOINT_VOLUME) != 0 ? "   [endpoint volume]"
                                                                  : "",
                    info.id);
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

    // How much of the container this tone actually uses. A quiet tone is safe
    // for headphones and proves correspondingly less about a 32-bit path, and
    // saying so is cheaper than someone assuming otherwise. The bit-exactness
    // proof is the capture test, not this.
    const std::uint32_t container_bits =
        8 * mp::container_bytes(negotiated.accepted.sample_type);
    const double peak = options.amplitude * (std::pow(2.0, container_bits - 1) - 1.0);
    const auto used = peak >= 1.0 ? static_cast<int>(std::floor(std::log2(peak))) + 2 : 0;
    std::printf("exercises  %d of %u bits\n\n", used, container_bits);

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


int decode(const MpDecoderVtbl& vtbl, const Options& options)
{
    if (options.file.empty()) {
        std::fprintf(stderr, "decode needs --file\n");
        return 1;
    }

    mp::Decoder decoder;
    const MpResult opened = decoder.open(vtbl, options.file.c_str());
    if (opened != MP_OK) {
        std::fprintf(stderr, "cannot decode %s: %s\n", options.file.c_str(),
                     result_name(opened));
        return 1;
    }

    const mp::Format format = decoder.format();
    const std::size_t stride = mp::frame_bytes(format);

    // std::filesystem::path rather than fopen: the path arrives as UTF-8, and a
    // narrow CRT call cannot open half the files on this machine.
    std::ofstream raw;
    if (!options.raw.empty()) {
        const std::filesystem::path where{
            std::u8string{reinterpret_cast<const char8_t*>(options.raw.c_str())}};
        raw.open(where, std::ios::binary);
        if (!raw) {
            std::fprintf(stderr, "cannot write %s\n", options.raw.c_str());
            return 1;
        }
    }

    mp::win::Sha256 hash;
    std::vector<std::uint8_t> chunk(64 * 1024 / stride * stride);
    std::uint64_t total = 0;
    for (;;) {
        const std::size_t got = decoder.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        hash.update(chunk.data(), got);
        if (raw.is_open()) {
            raw.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(got));
        }
        total += got;
    }
    raw.close();

    std::printf("file       %s\n", options.file.c_str());
    std::printf("format     %s\n", mp::describe(format).c_str());
    std::printf("frames     %llu (%.3f s)\n",
                static_cast<unsigned long long>(total / stride),
                static_cast<double>(total / stride) / format.sample_rate);
    if (decoder.length_frames() != 0 && decoder.length_frames() != total / stride) {
        std::printf("           header said %llu\n",
                    static_cast<unsigned long long>(decoder.length_frames()));
    }
    std::printf("bytes      %llu\n", static_cast<unsigned long long>(total));
    std::printf("sha256     %s\n", hash.hex().c_str());
    return 0;
}

/// A sink that forwards to a real one and keeps a copy of every byte committed.
///
/// This is the point where verification stops being possible to fake. The bytes
/// recorded here are the ones handed to `IAudioRenderClient::ReleaseBuffer` on a
/// real device: the last place any code of ours touches them, and exactly what
/// the bit-exactness claim is about. Past it is the driver, and no software on
/// this machine can see what the driver does -- docs/devices.md records the
/// measurement that made that concrete.
///
/// The copy is a `memcpy` into memory reserved before playback starts, so the
/// render thread still allocates nothing.
class TeeSink {
public:
    TeeSink(const MpSinkVtbl& inner, MpSink* handle) : inner_(&inner), handle_(handle)
    {
        // Every entry that takes a handle has to be wrapped, not just the two
        // that do something interesting. Copying the inner vtable and
        // overriding a subset leaves the rest pointing at the real module while
        // the *handle* is this object -- the module then reads its own struct
        // out of a TeeSink and the process dies at the first negotiate. An
        // access violation, once, is cheaper than the day it would have cost
        // later.
        vtbl_.size = sizeof(MpSinkVtbl);
        vtbl_.reserved = 0;
        vtbl_.enumerate = inner.enumerate; // no handle: safe to pass through
        vtbl_.open = inner.open;           // likewise
        vtbl_.negotiate = &TeeSink::negotiate_thunk;
        vtbl_.get_period = &TeeSink::get_period_thunk;
        vtbl_.start = &TeeSink::start_thunk;
        vtbl_.stop = &TeeSink::stop_thunk;
        vtbl_.close = &TeeSink::close_thunk;
        vtbl_.wait = &TeeSink::wait_thunk;
        vtbl_.acquire = &TeeSink::acquire_thunk;
        vtbl_.commit = &TeeSink::commit_thunk;
        vtbl_.get_position = &TeeSink::get_position_thunk;
    }

    /// The Sink owns the real device through this object: closing it closes the
    /// device.
    [[nodiscard]] mp::Sink sink() noexcept
    {
        return mp::Sink{&vtbl_, reinterpret_cast<MpSink*>(this)};
    }

    /// Called after negotiation, when the frame size is known and before
    /// anything starts, because the render thread may not allocate.
    void reserve(std::size_t bytes, std::uint32_t frame_bytes)
    {
        buffer_.assign(bytes, 0);
        used_ = 0;
        frame_bytes_ = frame_bytes;
        overflowed_ = false;
    }

    [[nodiscard]] std::size_t committed() const noexcept { return used_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return buffer_.data(); }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

private:
    static TeeSink& self(MpSink* s) noexcept { return *reinterpret_cast<TeeSink*>(s); }

    static MpResult MP_CALL negotiate_thunk(MpSink* s, const MpFormat* want, MpFormat* out)
    {
        TeeSink& me = self(s);
        return me.inner_->negotiate(me.handle_, want, out);
    }

    static MpResult MP_CALL get_period_thunk(MpSink* s, std::uint32_t* frames)
    {
        TeeSink& me = self(s);
        return me.inner_->get_period(me.handle_, frames);
    }

    static MpResult MP_CALL start_thunk(MpSink* s)
    {
        TeeSink& me = self(s);
        return me.inner_->start(me.handle_);
    }

    static MpResult MP_CALL stop_thunk(MpSink* s)
    {
        TeeSink& me = self(s);
        return me.inner_->stop(me.handle_);
    }

    static MpResult MP_CALL wait_thunk(MpSink* s, std::uint32_t timeout_ms)
    {
        TeeSink& me = self(s);
        return me.inner_->wait(me.handle_, timeout_ms);
    }

    static MpResult MP_CALL get_position_thunk(MpSink* s, std::uint64_t* frames,
                                               std::uint64_t* qpc)
    {
        TeeSink& me = self(s);
        return me.inner_->get_position(me.handle_, frames, qpc);
    }

    static MpResult MP_CALL acquire_thunk(MpSink* s, void** ptr, std::uint32_t* frames)
    {
        TeeSink& me = self(s);
        const MpResult r = me.inner_->acquire(me.handle_, ptr, frames);
        me.last_ = static_cast<const std::uint8_t*>(*ptr);
        return r;
    }

    static MpResult MP_CALL commit_thunk(MpSink* s, std::uint32_t frames, std::uint32_t flags)
    {
        TeeSink& me = self(s);
        const std::size_t bytes = static_cast<std::size_t>(frames) * me.frame_bytes_;
        if (me.last_ != nullptr && bytes != 0) {
            if (me.used_ + bytes <= me.buffer_.size()) {
                std::memcpy(me.buffer_.data() + me.used_, me.last_, bytes);
                me.used_ += bytes;
            } else {
                me.overflowed_ = true;
            }
        }
        return me.inner_->commit(me.handle_, frames, flags);
    }

    static void MP_CALL close_thunk(MpSink* s)
    {
        TeeSink& me = self(s);
        if (me.handle_ != nullptr && me.inner_->close != nullptr) {
            me.inner_->close(me.handle_);
        }
        me.handle_ = nullptr;
    }

    MpSinkVtbl vtbl_{};
    const MpSinkVtbl* inner_;
    MpSink* handle_;
    std::vector<std::uint8_t> buffer_;
    std::size_t used_ = 0;
    const std::uint8_t* last_ = nullptr;
    std::uint32_t frame_bytes_ = 0;
    bool overflowed_ = false;
};

/// A source over bytes already in memory, so `verify` measures the audio path
/// rather than the decoder's throughput.
class MemorySource final : public mp::ISource {
public:
    MemorySource(const mp::Format& format, const std::vector<std::uint8_t>& data)
        : format_(format), data_(&data)
    {
    }

    [[nodiscard]] const mp::Format& format() const noexcept override { return format_; }

    std::size_t read(void* dst, std::size_t bytes) override
    {
        const std::size_t stride = mp::frame_bytes(format_);
        const std::size_t left = data_->size() - offset_;
        const std::size_t n = std::min(bytes - (bytes % stride), left);
        std::memcpy(dst, data_->data() + offset_, n);
        offset_ += n;
        return n;
    }

private:
    mp::Format format_;
    const std::vector<std::uint8_t>* data_;
    std::size_t offset_ = 0;
};

/// A block that cannot be mistaken for anything in the music.
///
/// The capture starts whenever the driver starts it, so the recording has to be
/// lined up with what was played by searching for a known block. Using the start
/// of the file itself would not do: real music very often begins with digital
/// silence, and the first silence in the recording is not the right offset.
std::vector<std::uint8_t> sync_block(std::size_t frame_bytes)
{
    constexpr std::size_t target = 8192;
    const std::size_t bytes = (target / frame_bytes) * frame_bytes;
    std::vector<std::uint8_t> out(bytes);

    // A fixed 64-bit LCG. Deterministic, dense in every byte, and not silence.
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (auto& byte : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        byte = static_cast<std::uint8_t>(state >> 56);
    }
    return out;
}

int verify(const MpSinkVtbl& sink_vtbl, const MpDecoderVtbl& decoder_vtbl,
           const Options& options)
{
    if (options.file.empty()) {
        std::fprintf(stderr, "verify needs --file\n");
        return 1;
    }

    // ---- decode ----------------------------------------------------------
    mp::Decoder decoder;
    const MpResult opened = decoder.open(decoder_vtbl, options.file.c_str());
    if (opened != MP_OK) {
        std::fprintf(stderr, "cannot decode %s: %s\n", options.file.c_str(),
                     result_name(opened));
        return 1;
    }

    const mp::Format source_format = decoder.format();
    const std::size_t source_stride = mp::frame_bytes(source_format);
    const std::uint64_t cap_frames =
        static_cast<std::uint64_t>(options.seconds) * source_format.sample_rate;

    std::vector<std::uint8_t> source_bytes;
    source_bytes.reserve(static_cast<std::size_t>(cap_frames) * source_stride);
    {
        std::vector<std::uint8_t> chunk(64 * 1024 / source_stride * source_stride);
        while (source_bytes.size() < cap_frames * source_stride) {
            const std::size_t got = decoder.read(chunk.data(), chunk.size());
            if (got == 0) {
                break;
            }
            source_bytes.insert(source_bytes.end(), chunk.begin(),
                                chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
    }
    if (source_bytes.empty()) {
        std::fprintf(stderr, "the file decoded to nothing\n");
        return 1;
    }

    std::printf("file       %s\n", options.file.c_str());
    std::printf("decoded    %s, %zu frames, %.2f s\n",
                mp::describe(source_format).c_str(), source_bytes.size() / source_stride,
                static_cast<double>(source_bytes.size() / source_stride) /
                    source_format.sample_rate);
    std::printf("           %s\n",
                mp::win::Sha256::of(source_bytes.data(), source_bytes.size()).c_str());

    // ---- open the device, through the tee from the start ------------------
    std::string device_id;
    if (!device_id_for(sink_vtbl, options.device_index, device_id)) {
        return 1;
    }

    MpSink* raw_handle = nullptr;
    const MpResult device_opened =
        sink_vtbl.open(device_id.empty() ? nullptr : device_id.c_str(),
                       options.shared ? MP_SHARE_SHARED : MP_SHARE_EXCLUSIVE, &raw_handle);
    if (device_opened != MP_OK) {
        std::fprintf(stderr, "open failed: %s\n", result_name(device_opened));
        return 1;
    }

    TeeSink tee{sink_vtbl, raw_handle};
    mp::Sink sink = tee.sink(); // owns the device from here

    const auto negotiated = mp::negotiate_best(sink, source_format);
    if (!negotiated.ok) {
        std::fprintf(stderr, "no candidate was accepted (%zu offered): %s\n",
                     negotiated.tried, result_name(negotiated.last_error));
        return 1;
    }

    std::uint32_t period = 0;
    if (sink.period_frames(period) != MP_OK || period == 0) {
        std::fprintf(stderr, "the device did not report a buffer size\n");
        return 1;
    }

    // ---- what should reach the wire -------------------------------------
    const std::size_t wire_stride = mp::frame_bytes(negotiated.accepted);
    const std::size_t frames = source_bytes.size() / source_stride;

    std::vector<std::uint8_t> expected;
    if (negotiated.fidelity == mp::Fidelity::exact) {
        expected = source_bytes;
    } else {
        expected.resize(frames * wire_stride);
        if (!mp::repack(source_bytes.data(), source_format.sample_type, expected.data(),
                        negotiated.accepted.sample_type,
                        mp::effective_valid_bits(negotiated.accepted),
                        frames * source_format.channels)) {
            std::fprintf(stderr, "cannot repack into the accepted format\n");
            return 1;
        }
    }

    std::printf("wire       %s\n", mp::describe(negotiated.accepted).c_str());
    std::printf("path       %s, buffer %u frames\n",
                negotiated.fidelity == mp::Fidelity::exact ? "memcpy" : "container repack",
                period);

    // ---- the loopback, if it was asked for -------------------------------
    std::string capture_id;
    mp::win::Capture capture;
    bool capturing = false;
    if (options.loopback) {
        if (options.capture_index >= 0) {
            MpDeviceInfo info{};
            if (mp::win::Capture::enumerate(static_cast<std::uint32_t>(options.capture_index),
                                            info) != MP_OK) {
                std::fprintf(stderr, "no capture endpoint with index %d\n",
                             options.capture_index);
                return 1;
            }
            capture_id = info.id;
        } else {
            capture_id = mp::win::Capture::find_by_name("CABLE Output");
            if (capture_id.empty()) {
                std::fprintf(stderr, "no capture endpoint named \"CABLE Output\"\n");
                return 1;
            }
        }
        std::printf("loopback   render volume %.4f, capture volume %.4f\n",
                    mp::win::endpoint_volume(device_id, false),
                    mp::win::endpoint_volume(capture_id, true));
    }

    // ---- play ------------------------------------------------------------
    const auto sync = sync_block(wire_stride);
    std::vector<std::uint8_t> stream;
    stream.reserve(sync.size() + expected.size());
    stream.insert(stream.end(), sync.begin(), sync.end());
    stream.insert(stream.end(), expected.begin(), expected.end());

    // The stream, plus the silence the last partial buffer is padded with, plus
    // a few periods of slack.
    tee.reserve(stream.size() + 8 * period * wire_stride,
                static_cast<std::uint32_t>(wire_stride));

    MemorySource playback{negotiated.accepted, stream};
    mp::win::RenderThreadHooks hooks;
    mp::PassthroughGraph graph{playback, sink, negotiated.accepted, period,
                               mp::Fidelity::exact, &hooks};

    if (options.loopback) {
        if (capture.open(capture_id) != MP_OK) {
            std::fprintf(stderr, "cannot open the capture endpoint\n");
            return 1;
        }
        const MpResult ready = capture.negotiate(mp::to_abi(negotiated.accepted));
        if (ready != MP_OK) {
            std::fprintf(stderr, "the capture endpoint refused %s: %s\n",
                         mp::describe(negotiated.accepted).c_str(), result_name(ready));
            return 1;
        }
        if (capture.start(stream.size() * 2 + 4 * 1024 * 1024) != MP_OK) {
            std::fprintf(stderr, "cannot start the capture\n");
            return 1;
        }
        capturing = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    const MpResult started = graph.start();
    if (started != MP_OK) {
        if (capturing) {
            capture.stop();
        }
        std::fprintf(stderr, "could not start playback: %s\n", result_name(started));
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{static_cast<long long>(options.seconds) + 30};
    while (graph.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    graph.stop();
    if (capturing) {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        capture.stop();
    }

    const auto stats = graph.stats();
    std::printf("played     %llu frames, %llu underruns\n\n",
                static_cast<unsigned long long>(stats.frames_rendered),
                static_cast<unsigned long long>(stats.underruns));

    // ---- the comparison that decides ------------------------------------
    if (tee.overflowed()) {
        std::fprintf(stderr, "the tee ran out of room; nothing can be concluded\n");
        return 1;
    }
    if (tee.committed() < stream.size()) {
        std::fprintf(stderr, "only %zu of %zu bytes reached the device\n",
                     tee.committed(), stream.size());
        return 1;
    }

    const std::uint8_t* sent_to_device = tee.data() + sync.size();
    const std::string expected_hash = mp::win::Sha256::of(expected.data(), expected.size());
    const std::string committed_hash = mp::win::Sha256::of(sent_to_device, expected.size());

    std::printf("expected   %s\n", expected_hash.c_str());
    std::printf("committed  %s   <- handed to ReleaseBuffer\n\n",
                committed_hash.c_str());

    int status = 0;
    if (expected_hash == committed_hash) {
        std::printf("BIT-EXACT to the device buffer: %zu bytes, %s.\n", expected.size(),
                    negotiated.fidelity == mp::Fidelity::exact
                        ? "not one byte altered"
                        : "repacked into the accepted container and otherwise unaltered");
        std::printf("           Past ReleaseBuffer is the driver, which nothing on this\n");
        std::printf("           machine can observe. See docs/devices.md.\n");
    } else {
        std::size_t first = 0;
        while (first < expected.size() && expected[first] == sent_to_device[first]) {
            ++first;
        }
        std::printf("MISMATCH before the device. First differing byte %zu (frame %zu).\n",
                    first, first / wire_stride);
        status = 1;
    }

    // ---- the loopback, reported for what it is ---------------------------
    if (capturing) {
        std::printf("\nrecorded   %zu bytes, %llu discontinuities\n",
                    capture.data().size(),
                    static_cast<unsigned long long>(capture.discontinuities()));

        if (!options.raw.empty()) {
            const auto write = [](const std::string& where, const void* data,
                                  std::size_t bytes) {
                const std::filesystem::path p{
                    std::u8string{reinterpret_cast<const char8_t*>(where.c_str())}};
                std::ofstream out{p, std::ios::binary};
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            };
            write(options.raw + ".sent", stream.data(), stream.size());
            write(options.raw + ".recorded", capture.data().data(), capture.data().size());
            std::printf("dumped     %s.sent, %s.recorded\n", options.raw.c_str(),
                        options.raw.c_str());
        }

        const std::size_t at = mp::win::find_bytes(capture.data(), sync.data(), sync.size());
        if (at == std::string::npos) {
            std::printf("loopback   the sync block does not appear verbatim, so whatever\n");
            std::printf("           sits between the two endpoints did not pass the bytes\n");
            std::printf("           through. That is a statement about the loopback, not\n");
            std::printf("           about the player.\n");
        } else {
            const std::uint8_t* got = capture.data().data() + at + sync.size();
            const bool same = capture.data().size() >= at + sync.size() + expected.size() &&
                              std::memcmp(got, expected.data(), expected.size()) == 0;
            std::printf("loopback   %s\n", same
                                                ? "returned every byte unchanged"
                                                : "found the sync block but altered the payload");
        }
    }

    return status;
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
    if (options.command == "decode" || options.command == "verify") {
        const auto decoder_module =
            mp::win::LoadedModule::load(mp::win::module_directory() / "mp_decode_native.dll");
        if (!decoder_module || decoder_module->desc().kind != MP_KIND_DECODER) {
            std::fprintf(stderr, "could not load mp_decode_native.dll\n");
            return 1;
        }
        const auto* decoder_vtbl =
            static_cast<const MpDecoderVtbl*>(decoder_module->desc().vtbl);
        if (decoder_vtbl == nullptr || decoder_vtbl->size < sizeof(MpDecoderVtbl)) {
            std::fprintf(stderr, "the decoder module returned a vtable this host cannot read\n");
            return 1;
        }
        return options.command == "decode" ? decode(*decoder_vtbl, options)
                                          : verify(sink, *decoder_vtbl, options);
    }

    usage();
    return 1;
}

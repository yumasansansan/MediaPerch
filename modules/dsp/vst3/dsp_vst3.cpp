// SPDX-License-Identifier: GPL-3.0-or-later
//
// Somebody else's plugin, as a Path B stage.
//
// **The first module here that runs code this project did not write.** Every
// other stage in `modules/dsp` is a filter with its coefficients in the source;
// this one loads an arbitrary DLL and hands it the bus. That is worth saying at
// the top because it changes what the module can promise: a chain with a VST3
// in it is exactly as reproducible as the plugin is, and no claim this tree
// makes about bit-exactness survives contact with a binary it cannot read.
//
// What it *can* promise is the shape. The plugin is given the stream's real
// rate and channel layout, a block no larger than the one it agreed to, and
// f64 samples when it says it can take them -- which this tree's bus already
// is, so a plugin that processes in double precision is fed with no conversion
// at all. `describe` says which of the two happened, along with the latency and
// the tail the plugin declared, because those are the numbers that decide
// whether the audio ends up where it should.
//
// The hosting is in vst3_host.cpp. This file is the vtable and the settings.

#include "vst3_host.hpp"

#include <mediaperch/module.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

/// Reads a whole file, for `state`. Small by nature -- a VST3 state is a
/// preset, not a sample library -- and capped so that pointing this at the
/// wrong file is an error rather than an allocation.
constexpr std::size_t k_max_state = 64u * 1024u * 1024u;

bool read_file(const char* path, std::vector<std::uint8_t>& out, std::string& why)
{
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
        why = std::string{"cannot open "} + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0 || static_cast<std::size_t>(size) > k_max_state) {
        std::fclose(f);
        why = std::string{path} + " is not a plugin state (it is " + std::to_string(size) +
              " bytes)";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        why = std::string{"cannot read "} + path;
        return false;
    }
    return true;
}

} // namespace

struct MpDsp {
    mp::vst3::Host host;
    /// What was asked for, kept so that `describe` can say it back and so that
    /// `configure` can load lazily: settings arrive before the format does, and
    /// a plugin cannot be told about a stream that has not been described yet.
    std::string path;
    std::string which;
    std::string state_path;
    /// `name=value` pairs, applied after loading and remembered so that a
    /// reconfigure does not silently lose them.
    std::vector<std::pair<std::string, double>> parameters;
    /// Why the last thing that failed, failed. `describe` shows it, because a
    /// stage that quietly does nothing is the worst kind.
    std::string trouble;

    MpFormat format{};
    std::uint32_t max_frames = 0;
    /// The tail, in frames, that `flush` still owes. A reverb keeps going after
    /// the file stops and that decay is as much the output as the rest.
    std::uint32_t tail_left = 0;
    /// Deinterleaved views into the caller's buffers, rebuilt each block. The
    /// ABI hands over arrays of pointers already, so this is only the const
    /// removed and the silence for a drain.
    std::vector<double> drain_in;
    std::vector<const double*> drain_ptr;
    bool loaded = false;
};

namespace {

/// Loads and configures, if it has been told enough to. Called from `configure`
/// rather than from `set` because a plugin needs the format, and the format is
/// the last thing to arrive.
bool bring_up(MpDsp* d)
{
    if (d->path.empty()) {
        d->trouble = "no plugin: give it one with --dsp vst3:file=<path to .vst3>";
        return false;
    }
    if (!d->loaded) {
        if (!d->host.load(d->path, d->which, d->trouble)) {
            return false;
        }
        d->loaded = true;
        if (!d->state_path.empty()) {
            std::vector<std::uint8_t> bytes;
            if (!read_file(d->state_path.c_str(), bytes, d->trouble) ||
                !d->host.set_state(bytes, d->trouble)) {
                return false;
            }
        }
    }
    if (!d->host.configure(d->format.channels, d->format.channel_mask,
                           static_cast<double>(d->format.sample_rate), d->max_frames,
                           d->trouble)) {
        return false;
    }
    // Parameters last: `configure` may have restarted the plugin, and a
    // restarted plugin is back at its defaults.
    for (const auto& [name, value] : d->parameters) {
        if (!d->host.set_parameter(name, value, d->trouble)) {
            return false;
        }
    }
    d->trouble.clear();
    return true;
}

MpResult MP_CALL dsp_open(MpDsp** out) noexcept
{
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = new (std::nothrow) MpDsp();
    return *out != nullptr ? MP_OK : MP_ERR_NO_MEMORY;
}

void MP_CALL dsp_close(MpDsp* d) noexcept
{
    delete d;
}

MpResult MP_CALL dsp_configure(MpDsp* d, const MpFormat* in, std::uint32_t max_frames,
                               MpFormat* out, std::uint32_t* out_max) noexcept
{
    if (d == nullptr || in == nullptr || out == nullptr || out_max == nullptr) {
        return MP_ERR_INVALID;
    }
    if (in->channels == 0 || in->channels > 64 || max_frames == 0 || in->sample_rate == 0) {
        return MP_ERR_FORMAT;
    }
    d->format = *in;
    d->max_frames = max_frames;
    d->tail_left = 0;

    if (!bring_up(d)) {
        return MP_ERR_FORMAT;
    }

    // A VST3 effect gives back one frame for every frame it is given: the
    // latency it declares is a delay through the filter, not a change in count.
    // Everything interesting -- a plugin that resamples internally, one that
    // reports latency -- still comes out one for one, which is why this stage
    // is one of the simple ones for the host to schedule.
    *out = *in;
    *out_max = max_frames;

    const std::size_t block = static_cast<std::size_t>(in->channels) * max_frames;
    d->drain_in.assign(block, 0.0);
    d->drain_ptr.resize(in->channels);
    for (std::uint32_t c = 0; c < in->channels; ++c) {
        d->drain_ptr[c] = d->drain_in.data() + static_cast<std::size_t>(c) * max_frames;
    }
    return MP_OK;
}

MpResult MP_CALL dsp_process(MpDsp* d, const double* const* in, std::uint32_t in_frames,
                             double* const* out, std::uint32_t out_capacity,
                             std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    if (in_frames == 0 || in == nullptr) {
        return MP_OK;
    }
    if (in_frames > out_capacity) {
        return MP_ERR_INVALID;
    }
    if (!d->host.active()) {
        return MP_ERR_INVALID;
    }
    if (!d->host.process(in, in_frames, out)) {
        d->trouble = d->host.name() + " failed to process a block";
        return MP_ERR_INTERNAL;
    }
    // The tail starts full and is spent by `flush`: a plugin that has seen
    // audio has that much of it still inside.
    d->tail_left = d->host.tail_frames();
    *out_frames = in_frames;
    return MP_OK;
}

MpResult MP_CALL dsp_flush(MpDsp* d, double* const* out, std::uint32_t out_capacity,
                           std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = 0;
    if (d->tail_left == 0 || out == nullptr || out_capacity == 0 || !d->host.active()) {
        return MP_OK;
    }
    // An infinite tail is a delay line with feedback at unity, or a plugin that
    // did not think about the question. Either way something has to choose a
    // stopping point, and silence is not an answer that ever arrives: one
    // second is long enough for every real decay and short enough that a file
    // does not grow a minute of nothing.
    if (d->tail_left == mp::vst3::Host::k_infinite_tail) {
        d->tail_left = d->format.sample_rate;
    }
    const std::uint32_t n = std::min(d->tail_left, out_capacity);
    if (!d->host.process_silence(n, out)) {
        d->tail_left = 0;
        return MP_OK;
    }
    d->tail_left -= n;
    *out_frames = n;
    return MP_OK;
}

MpResult MP_CALL dsp_reset(MpDsp* d) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    d->tail_left = 0;
    return d->host.reset() ? MP_OK : MP_ERR_INTERNAL;
}

MpResult MP_CALL dsp_get_latency(MpDsp* d, std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = d->host.latency_frames();
    return MP_OK;
}

MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "file") == 0) {
        d->path = value;
        d->loaded = false; // a new plugin is not the old one reconfigured
        return MP_OK;
    }
    if (std::strcmp(key, "class") == 0) {
        d->which = value;
        d->loaded = false;
        return MP_OK;
    }
    if (std::strcmp(key, "state") == 0) {
        d->state_path = value;
        d->loaded = false;
        return MP_OK;
    }
    if (std::strcmp(key, "param") == 0) {
        // `name=value`, normalised. Appended rather than replaced, because a
        // plugin has more than one knob and each one is a separate `--dsp`
        // setting on a command line that has already been split on commas.
        const char* equals = std::strrchr(value, '=');
        if (equals == nullptr || equals == value) {
            return MP_ERR_INVALID;
        }
        char* end = nullptr;
        const double normalised = std::strtod(equals + 1, &end);
        if (end == equals + 1 || !(normalised >= 0.0 && normalised <= 1.0)) {
            return MP_ERR_INVALID;
        }
        const std::string name{value, static_cast<std::size_t>(equals - value)};
        d->parameters.emplace_back(name, normalised);
        // Applied at once when the plugin is already running, so that a live
        // change is a live change; `bring_up` replays them after a reconfigure.
        if (d->host.active() && !d->host.set_parameter(name, normalised, d->trouble)) {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
}

MpResult MP_CALL dsp_describe(MpDsp* d, std::uint32_t index, char* out,
                              std::uint32_t out_bytes) noexcept
{
    if (d == nullptr || out == nullptr || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes, "file\t%s\tthe .vst3 to load, a DLL or a bundle",
                      d->path.empty() ? "none" : d->path.c_str());
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes,
                      "class\t%s\twhich audio effect in it: an index, or part of a name",
                      d->which.empty() ? "(first)" : d->which.c_str());
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "state\t%s\ta file of plugin state, as setState takes it",
                      d->state_path.empty() ? "none" : d->state_path.c_str());
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "param\t(append)\tone knob, as name=value or id=value, normalised 0 to 1");
        return MP_OK;
    case 4:
        std::snprintf(out, out_bytes, "plugin\t%s\twhat loaded (read only)",
                      d->host.loaded() ? d->host.name().c_str() : "nothing");
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes, "vendor\t%s %s\twho wrote it (read only)",
                      d->host.vendor().c_str(), d->host.version().c_str());
        return MP_OK;
    case 6:
        std::snprintf(out, out_bytes, "category\t%s\twhat it says it is (read only)",
                      d->host.subcategories().c_str());
        return MP_OK;
    case 7:
        // The line worth reading. A plugin that takes doubles is handed this
        // tree's bus untouched; one that does not gets f32, and the samples are
        // narrowed and widened again around it.
        std::snprintf(out, out_bytes,
                      "precision\t%s\twhat the plugin is fed (read only)",
                      !d->host.loaded()  ? "-"
                      : d->host.native_f64() ? "f64, the bus itself"
                                             : "f32, converted both ways");
        return MP_OK;
    case 8:
        std::snprintf(out, out_bytes,
                      "latency\t%u\tframes it delays by, which nothing compensates yet "
                      "(read only)",
                      d->host.latency_frames());
        return MP_OK;
    case 9:
        if (d->host.tail_frames() == mp::vst3::Host::k_infinite_tail) {
            std::snprintf(out, out_bytes,
                          "tail\tinfinite\tit never stops; flush takes one second (read only)");
        } else {
            std::snprintf(out, out_bytes,
                          "tail\t%u\tframes it keeps producing after the input stops "
                          "(read only)",
                          d->host.tail_frames());
        }
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes, "trouble\t%s\twhat went wrong (read only)",
                      d->trouble.empty() ? "nothing" : d->trouble.c_str());
        return MP_OK;
    default:
        break;
    }
    // Then every parameter the plugin has, which is the only way to find out
    // what to put in `param=` without opening an editor this host does not have.
    const std::uint32_t at = index - 11;
    if (at >= d->host.parameter_count()) {
        return MP_END;
    }
    std::string title;
    std::string shown;
    double value = 0.0;
    if (!d->host.parameter(at, title, value, shown)) {
        return MP_END;
    }
    std::snprintf(out, out_bytes, "  %s\t%.4f\t%s (read only)", title.c_str(), value,
                  shown.c_str());
    return MP_OK;
}

const MpDspVtbl g_vtbl = {
    /* size        */ sizeof(MpDspVtbl),
    /* reserved    */ 0,
    /* open        */ &dsp_open,
    /* close       */ &dsp_close,
    /* configure   */ &dsp_configure,
    /* process     */ &dsp_process,
    /* flush       */ &dsp_flush,
    /* set         */ &dsp_set,
    /* describe    */ &dsp_describe,
    /* reset       */ &dsp_reset,
    /* get_latency */ &dsp_get_latency,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DSP,
    /* priority    */ 50,
    /* id          */ "dsp_vst3",
    /* name        */ "VST3 (somebody else's plugin, in this chain)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

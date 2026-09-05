// SPDX-License-Identifier: GPL-3.0-or-later

#include "impulse.hpp"

#include <dr_wav.h>
#include <resample.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace mp::impulse {
namespace {

constexpr double k_pi = 3.14159265358979323846;

#if defined(_WIN32)
std::wstring widen(const std::string& utf8)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
    return wide;
}
#endif

} // namespace

bool load(const std::string& path, Response& out, std::string& why)
{
    out = Response{};

    // Zeroed, so that the path where opening fails before drwav_init touches
    // it is not a read of an indeterminate struct -- which it never was, and
    // which the compiler cannot see.
    drwav wav{};
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    const drwav_bool32 opened =
        wide.empty() ? DRWAV_FALSE : drwav_init_file_w(&wav, wide.c_str(), nullptr);
#else
    const drwav_bool32 opened = drwav_init_file(&wav, path.c_str(), nullptr);
#endif
    if (opened == DRWAV_FALSE) {
        why = "could not read " + path + " as an impulse response";
        return false;
    }

    if (wav.channels == 0 || wav.channels > 64 || wav.sampleRate == 0) {
        drwav_uninit(&wav);
        why = "that file says it has no channels or no rate";
        return false;
    }
    // Ten minutes at 192 kHz. Past any impulse response and short of anything
    // that would exhaust memory by accident.
    constexpr drwav_uint64 k_limit = 115'200'000;
    if (wav.totalPCMFrameCount == 0 || wav.totalPCMFrameCount > k_limit) {
        const bool empty = wav.totalPCMFrameCount == 0;
        drwav_uninit(&wav);
        why = empty ? "that file has no samples in it"
                    : "that file is longer than any impulse response should be";
        return false;
    }

    const auto frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
    const std::uint32_t channels = wav.channels;
    std::vector<float> interleaved(frames * channels);
    const drwav_uint64 read =
        drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, interleaved.data());
    out.sample_rate = wav.sampleRate;
    drwav_uninit(&wav);
    if (read != frames) {
        why = "that file ended before it said it would";
        return false;
    }

    out.channels.assign(channels, std::vector<double>(frames, 0.0));
    for (std::uint32_t c = 0; c < channels; ++c) {
        std::vector<double>& plane = out.channels[c];
        for (std::size_t n = 0; n < frames; ++n) {
            plane[n] = interleaved[n * channels + c];
        }
    }
    return true;
}

Gains measure(const Response& response) noexcept
{
    Gains out;
    out.dc = 0.0;
    for (const std::vector<double>& plane : response.channels) {
        double dc = 0.0;
        double peak = 0.0;
        double energy = 0.0;
        for (const double tap : plane) {
            dc += tap;
            peak = std::max(peak, std::abs(tap));
            energy += tap * tap;
        }
        // The worst channel is what a caller has to have room for.
        out.dc = std::abs(dc) > std::abs(out.dc) ? dc : out.dc;
        out.peak = std::max(out.peak, peak);
        out.energy = std::max(out.energy, std::sqrt(energy));
    }
    return out;
}

void scale(Response& response, double factor) noexcept
{
    for (std::vector<double>& plane : response.channels) {
        for (double& tap : plane) {
            tap *= factor;
        }
    }
}

void truncate(Response& response, std::size_t max_taps) noexcept
{
    if (max_taps == 0 || response.frames() <= max_taps) {
        return;
    }
    // A hundredth of the kept length, and at least sixty-four samples: enough
    // that the cut is a fade rather than a step, and short enough that it does
    // not change what the response is.
    const std::size_t fade = std::max<std::size_t>(64, max_taps / 100);
    for (std::vector<double>& plane : response.channels) {
        plane.resize(max_taps);
        const std::size_t from = max_taps > fade ? max_taps - fade : 0;
        for (std::size_t n = from; n < max_taps; ++n) {
            const double t =
                static_cast<double>(n - from) / static_cast<double>(max_taps - from);
            plane[n] *= 0.5 * (1.0 + std::cos(k_pi * t)); // raised cosine, to zero
        }
    }
}

bool resample_to(Response& response, std::uint32_t rate, std::string& why)
{
    if (response.empty() || rate == 0) {
        why = "nothing to resample";
        return false;
    }
    if (response.sample_rate == rate) {
        return true;
    }

    // The best setting there is. This runs once, at configure, over a few
    // hundred thousand samples -- there is no argument for doing it worse.
    mp::resample::Design design;
    if (!mp::resample::design_from_name("best", design)) {
        why = "the resampler has no `best`";
        return false;
    }
    design.stages = 0; // let it split the ratio if that is cheaper

    const auto channels = static_cast<std::uint32_t>(response.channels.size());
    const auto frames = static_cast<std::uint32_t>(response.frames());

    mp::resample::Cascade cascade;
    if (!cascade.configure(response.sample_rate, rate, channels, frames, design, why)) {
        return false;
    }

    std::vector<const double*> in(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        in[c] = response.channels[c].data();
    }
    const std::uint32_t capacity = cascade.max_output(frames) + 16;
    std::vector<std::vector<double>> scratch(channels, std::vector<double>(capacity, 0.0));
    std::vector<double*> out(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        out[c] = scratch[c].data();
    }

    std::vector<std::vector<double>> converted(channels);
    std::uint32_t produced = 0;
    if (!cascade.process(in.data(), frames, out.data(), capacity, produced)) {
        why = "the resampler refused the impulse response";
        return false;
    }
    for (std::uint32_t c = 0; c < channels; ++c) {
        converted[c].insert(converted[c].end(), scratch[c].begin(),
                            scratch[c].begin() + static_cast<std::ptrdiff_t>(produced));
    }
    for (int round = 0; round < 4096; ++round) {
        std::uint32_t made = 0;
        if (!cascade.flush(out.data(), capacity, made)) {
            why = "the resampler refused to drain";
            return false;
        }
        if (made == 0) {
            break;
        }
        for (std::uint32_t c = 0; c < channels; ++c) {
            converted[c].insert(converted[c].end(), scratch[c].begin(),
                                scratch[c].begin() + static_cast<std::ptrdiff_t>(made));
        }
    }

    // **The gain has to be put back.** A resampler preserves the *signal*, so
    // the impulse spreads over more samples and its taps sum to more of them:
    // resampling a wire from 44.1 to 88.2 kHz gives something whose taps add up
    // to two. A filter's gain is the sum of its taps at any rate, so the ratio
    // is divided back out -- without this, a 44.1 kHz response on a 96 kHz
    // stream is 6.7 dB loud, which sounds like a resampler bug and is not one.
    const double ratio =
        static_cast<double>(response.sample_rate) / static_cast<double>(rate);
    response.channels = std::move(converted);
    response.sample_rate = rate;
    if (ratio != 1.0) {
        scale(response, ratio);
    }
    return true;
}

bool fit_channels(Response& response, std::uint32_t channels, std::string& why)
{
    if (response.channels.empty() || channels == 0) {
        why = "nothing to fit";
        return false;
    }
    if (response.channels.size() == channels) {
        return true;
    }
    if (response.channels.size() == 1) {
        // Copied out first: `assign` destroys what is there before it fills,
        // and `front()` is a reference into exactly that.
        const std::vector<double> one = response.channels.front();
        response.channels.assign(channels, one);
        return true;
    }
    // A four-channel file against a stereo stream is somebody's true-stereo
    // matrix -- left into left *and* right, right into both -- and that is a
    // different convolution with a different meaning. Refusing is the only
    // honest answer until it is written.
    why = "that response has " + std::to_string(response.channels.size()) +
          " channels and the stream has " + std::to_string(channels) +
          "; one response is applied to every channel and " +
          std::to_string(channels) + " are applied one to one, but nothing else is";
    return false;
}

} // namespace mp::impulse

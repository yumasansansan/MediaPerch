// SPDX-License-Identifier: GPL-3.0-or-later
//
// mediaperch-probe: milestone 1's whole user interface.
//
// Three commands, and the difference between them is how much they disturb the
// machine. Listing devices opens nothing. The other two take the endpoint, which
// in exclusive mode silences every other application on it -- so each says so
// before it does it.

#include "mediaperch/platform.hpp"

#include "mediaperch/display.hpp"
#include "mediaperch/display_win.hpp"
#include "mediaperch/video.hpp"

#include "mediaperch/compare.hpp"
#include "mediaperch/dsp.hpp"
#include "mediaperch/engine_host.hpp"
#include "mediaperch/log.hpp"
#include "mediaperch/packet.hpp"
#include "mediaperch/result.hpp"
#include "mediaperch/queue.hpp"
#if MEDIAPERCH_HAS_LOUDNESS
#    include <loudness.hpp>
#endif
#include "mediaperch/negotiation.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/processed.hpp"
#include "mediaperch/processor.hpp"
#include "mediaperch/shaper_tables.hpp"
#include "mediaperch/repack.hpp"
#include "mediaperch/sine.hpp"
#include "mediaperch/sink.hpp"
#include "mediaperch/verify.hpp"

#include <atomic>
#include <chrono>
#include <conio.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string command = "devices";
    std::string file;
    /// `--file` repeated. One is a track; more than one is a playlist, and the
    /// queue is what makes the join between them silent.
    std::vector<std::string> files;
    /// Keys during playback, for the transport that now exists.
    bool interactive = false;
    /// `show` only: do not open a sink, and pace the picture on the wall
    /// clock. For a file with no audio, and for a machine whose device
    /// refuses every format -- which is a real thing and not a hypothetical.
    bool no_audio = false;
    std::string raw;
    std::string decoder_id; // empty = let the probe decide
    std::string capture; // capture endpoint id; empty = find one by name
    int capture_index = -1;
    int device_index = -1; // -1 = the system default
    /// Part of an endpoint's name, matched case-insensitively. An alternative
    /// to the index, which changes when a device is plugged in.
    std::string device_name;
    std::uint32_t rate = 44100;
    std::uint32_t bits = 16;
    /// Whether `--bits` was actually given. `decode` quantises to the source's
    /// own container unless somebody named another one; `tone` has no source
    /// and needs the default.
    bool bits_given = false;
    std::uint32_t channels = 2;
    double hz = 1000.0;
    double amplitude = 0.5;
    unsigned seconds = 10;
    /// Whether `--seconds` was actually asked for. A file plays to its end
    /// unless somebody said otherwise; a tone has no end and needs the default.
    bool seconds_given = false;
    std::uint64_t seek = 0;
    mp::PathPolicy path = mp::PathPolicy::bit_exact;
    double gain = 1.0;
    /// `loudness`: what the ReplayGain figure is measured against.
    double target_lufs = -18.0;
    mp::DitherKind dither = mp::DitherKind::triangular;
    mp::NoiseShaping shaping{};
    /// The dither generator's seed. A setting because reproducibility is one,
    /// and because two runs that differ only in this are the cleanest way to
    /// hear what dither is doing.
    std::uint32_t dither_seed = 0x1f2e3d4cu;
    /// The ring, in device periods, and how long the render thread waits for a
    /// device that has stopped signalling. Both were constants until somebody
    /// with a busier machine needed the first one bigger.
    std::uint32_t ring_periods = 8;
    std::uint32_t wait_timeout_ms = 2000;
    /// Whether a device that goes away mid-run is the end of the run, and how
    /// long to wait for one to come back before agreeing that it is.
    bool recover = true;
    unsigned recover_timeout = 30;
    /// `name` or `name:key=value,key=value`, in the order given, which is the
    /// order they run in.
    std::vector<std::string> dsp;
    /// `decode` only: the block the chain is handed, standing in for the period
    /// a device would have named. A setting because the answer must not depend
    /// on it -- two runs at different blocks that hash differently mean a stage
    /// is carrying something it should not.
    std::uint32_t block = 4096;
    bool float_source = false;
    std::string source;          // `compare`: the audio that was encoded
    std::string rival_id = "demux_ffmpeg"; // and a second decoder to sit beside
    double min_snr_db = 0.0;
    double band_tol_db = 0.0;
    double min_lag_margin_db = 6.0;
    double min_channel_margin_db = 6.0;
    double max_peak_ratio = 2.0;
    double vs_rival_db = 0.0;
    std::uint32_t band_limit_hz = 0;
    int lag_window = 2048;
    bool exact_length = true;
    bool shared = false;
    /// Which sink module, by id. Empty means the highest-priority one, which is
    /// `sink_wasapi`. `sink_asio` is asked for by name because it takes a whole
    /// device, needs a driver, and buys nothing for PCM.
    std::string sink_id;
    bool loopback = false;
    bool verbose = false;
};

/// Every dither and every shaper, in the words the flags take.
///
/// A setting whose values can only be found by reading the source is a setting
/// nobody will use. There are 79 measured curves in this program, from two
/// projects, and this is the only place that says so.
void list_algorithms()
{
    std::printf(R"(--dither KIND
  none            no dither. The error is then a function of the signal, which
                  is what makes truncation sound like distortion rather than
                  like noise
  rectangular     one uniform LSB. No distortion, but a noise floor that
                  breathes with the signal
  triangular      two summed uniforms, +/- one LSB. THE DEFAULT and the
                  standard answer: no distortion and no modulation either
  highpass        the same distribution, formed from two samples one apart, so
                  it arrives tilted away from the midband
  gaussian        +/- half an LSB of Gaussian noise. The right shape when
                  something downstream will quantise again

--shape WHICH
  0               none. The error stays where it fell
)");
    std::printf("  1 .. %u          binomial: noise transfer function (1 - z^-1)^N exactly,\n",
                mp::NoiseShaping::k_max_order);
    std::printf(R"(                  derived rather than tabulated. **Order is not quality** --
                  each order moves noise further out of the midband and
                  multiplies the total, so 1 to 3 are useful and the rest are
                  the mechanism rather than a recommendation

  Measured curves, from ReSampler (jniemann66, LGPL-2.1). Fitted at 44.1 kHz
  and usable at other rates, where the shape stretches with the rate and the
  notches move off the frequencies they were placed at:

)");
    for (const auto& curve : mp::shaper_curves()) {
        if (curve.sample_rate != 0) {
            continue;
        }
        const std::string_view full{curve.name};
        const std::size_t colon = full.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view what = full.substr(colon + 2);
        std::printf("  %-14.*s  %.*s (%u tap%s)\n", static_cast<int>(colon), full.data(),
                    static_cast<int>(what.size()), what.data(), curve.length,
                    curve.length == 1 ? "" : "s");
    }

    std::printf(R"(
  shibata[:N]     ATH-weighted, from SSRC (shibatch, Boost 1.0). Weighted by the
                  absolute threshold of hearing, so they are fitted per sample
                  rate and are never substituted across one. N is SSRC's own
                  numbering, and the default is the highest this rate has:

                    0-6    ATH curve A, gentle to aggressive
                    10-16  ATH curve B
                    90-92  the older curves: low, mid, high
                    98     a plain first-order shaper
                    99     none at all

                  What each rate actually has:

)");
    std::uint32_t rate = 0;
    for (const auto& curve : mp::shaper_curves()) {
        if (curve.sample_rate == 0) {
            continue;
        }
        if (curve.sample_rate != rate) {
            if (rate != 0) {
                std::printf("\n");
            }
            rate = curve.sample_rate;
            std::printf("    %6u Hz   ", rate);
        }
        std::printf("%u ", curve.intensity);
    }
    if (rate != 0) {
        std::printf("\n");
    }
}

void usage()
{
    std::puts(R"(mediaperch-probe -- milestone 1

  devices     list render endpoints. Opens nothing and disturbs nothing.
  negotiate   offer every candidate format to a device for real.
              TAKES THE ENDPOINT briefly. In exclusive mode that silences
              every other application on it.
  play        play a test tone.
              TAKES THE ENDPOINT for the whole duration.
  modules     list what loaded, and what each one claims to be
  claims      who claims one file, and what is inside it: every demuxer's probe
              score, the streams each one finds, and the codec module that takes
              each stream
  decode      decode a file and print SHA-256 of the PCM it produced. Touches no
              device. Compare it with a reference decoder to check this one.
              With --path processed, --gain or --dsp it runs Path B instead --
              the same arithmetic a device would have got, hashed rather than
              played, which is the only way the resampler, the dither and the
              shapers can be held to anything without a sound card.
  compare     decode a file and hold the result against the audio that was
              encoded: length, alignment, channel order, fidelity and band
              energies. Touches no device. Exits non-zero if a requirement is
              not met, so it can be a test.
  verify      play a file into a loopback and record the other end, then compare
              SHA-256 of what was sent with SHA-256 of what came back.
              TAKES TWO ENDPOINTS -- a render one and a capture one -- and is
              only meaningful when they are the two halves of a virtual cable.

Options
  --file PATH       the file to decode. WAV and FLAC
  --raw PATH        `decode` writes the decoded PCM here. `verify` writes what it
                    sent to PATH.sent and what it recorded to PATH.recorded
  --decoder ID      force a container reader: a short name is a demuxer, so
                    `wav` is demux_wav and `mf` is demux_mf. Default is whichever
                    probes highest, ties broken by declared priority
  --device N        index from `devices`; default is the system default endpoint
  --capture N       capture endpoint index; default is one named "CABLE Output"
  --rate R          default 44100
  --bits 16|24|32   24 means 24 valid bits in a 32-bit container; default 16
  --float           the source is 32-bit float rather than integer. This is what
                    every lossy decoder here produces, and asking a device for it
                    is the question those files raise
  --channels N      default 2
  --hz F            tone frequency, default 1000
  --amplitude A     fraction of full scale, default 0.5 (-6 dBFS).
                    Turn this down for headphones: 0.02 is about -34 dBFS.
                    This is a property of the generator, not a gain stage: the
                    tone is computed and quantised once, and nothing scales a
                    sample afterwards. It does decide how much of the container
                    the tone exercises, which `play` reports.
  --seconds N       play duration, default 10
  --source PATH     `compare`: the uncompressed file that was encoded
  --rival ID        `compare`: a second decoder to measure beside this one, or
                    `none`. Default decode_ffmpeg
  --min-snr DB      `compare`: the fidelity floor against the source. Depends on
                    the bit rate, so there is no useful default
  --band-limit HZ   `compare`: check band energies up to here. 0 skips it
  --band-tol DB     `compare`: how far a band may sit from the source's
  --min-lag-margin DB   `compare`: how far the correlation peak must stand above
                    every lag outside its own shoulder. Guards against a test
                    signal that cannot locate itself
  --min-channel-margin DB  `compare`: likewise for telling channels apart
  --vs-rival DB     `compare`: how closely this decoder must agree with the rival
                    *directly*, sample for sample. The source cannot check this:
                    two decoders can differ and both stay the same distance from
                    the audio that was encoded
  --max-peak-ratio R    `compare`: how far past the source's peak the decode may
                    go. A codec quantising hard overshoots, and at 32 kbps it
                    overshoots by more than twice. Default 2
  --lag-window N    `compare`: frames either way to search for the start.
                    Default 2048, two AAC frames
  --untrimmed       `compare`: the decode may be longer than the source, which
                    is correct for a format with no gapless metadata
  --path WHICH      which graph a stream may take:
                      bitexact  a memcpy or a container repack, and nothing
                                else. THE DEFAULT: it can refuse, which is what
                                makes the others honest
                      exact     a memcpy only. Not even a repack
                      auto      bit-exact if the device will take it, and Path B
                                if it will not. Says loudly when it converts
                      processed Path B whether or not Path A was available
  --gain G          Path B only: linear gain, 1.0 is unity. A volume control has
                    nowhere else to live on an exclusive-mode stream. Asking for
                    one that is not unity implies --path processed, because a
                    gain is a change no format relationship can show
  --block N         `decode` only: frames per block through the chain, standing
                    in for the period a device would have named. Default 4096.
                    Two runs at different blocks that hash differently mean a
                    stage is carrying something across a boundary it should not
  --dsp STAGE       Path B only: add a stage to the chain, in the order given,
                    as `name` or `name:key=value,key=value`. Repeatable.
                    `--dsp list` prints every stage that is loaded, with every
                    setting it has and what that setting is now. A stage exists
                    to change the samples, so asking for one implies
                    --path processed. The one most people want is
                      --dsp resample:rate=48000[,quality=fast|good|best|extreme]
                    which is the only way a rate the device refuses ever gets
                    changed: nothing here resamples on its own. Its filter is
                    designed rather than tabulated, and how it is designed is
                    also a setting -- design=window|remez|refine,
                    window=kaiser|dpss|dolph, phase=linear|minimum,
                    stages=1|auto, attenuation, bandwidth, passband_ripple,
                    taps, cepstrum, phase_floor, verify. `--dsp list` prints them all with what each
                    one is now, and what the filter it built measured.
                    `--dsp mix:channels=2` is the other geometry: every
                    coefficient of the matrix is a setting, the matrix that was
                    built is reported, and an upmix places what exists rather
                    than inventing what does not. `--dsp eq:preset=<path>`
                    reads an AutoEq or Equalizer APO profile -- either the
                    parametric one or the graphic curve, which needs
                    mode=linear or mode=minimum because a curve is a target
                    rather than a cascade. `--dsp convolve:file=<path>` takes an
                    impulse response somebody measured, resamples it to the
                    stream's rate, and reports what gain it has before it
                    applies it
  --dither KIND     Path B only, and only where bits are actually being thrown
                    away. `triangular` (default) is the standard answer;
                    `highpass` is the same distribution with its noise tilted
                    away from the midband; `gaussian` is the right shape when
                    something downstream will quantise again; `rectangular`
                    leaves a noise floor that breathes with the signal;
                    `none` is what a measurement wants
  --shape WHICH     Path B only: a binomial order 0 to 9, `shibata[:N]` for a
                    measured ATH-weighted curve, or a published one by name.
                    `--shape list` (or `--dither list`) prints all of them
  --target LUFS     `loudness`: what the ReplayGain figure aims at. -18 is
                    ReplayGain 2.0's own reference
  --seek FRAMES     `decode` seeks here first, then hashes the rest. The hash of
                    a seek to N must equal the hash of the last (length - N)
                    frames of a straight decode, which is what makes seeking
                    testable at all
  --device-name S   the endpoint whose name contains S, case-insensitively.
                    Refuses rather than guesses when two of them match, because
                    opening the wrong endpoint in exclusive mode takes that
                    device away from everything else on the machine
  --dither-seed N   the dither generator's seed. Fixed rather than drawn from a
                    clock, so two decodes of one file produce the same bytes;
                    change it to hear the same setting a second time
  --ring-periods N  ring capacity in device periods, default 8. At a 3 ms period
                    that is 24 ms of slack for a decode thread that is not
                    real-time, and a busy machine may want more
  --wait-timeout MS how long the render thread waits for a device that has
                    stopped signalling before calling it gone. Default 2000
  --no-recover      treat a device that disappears mid-run as the end of the
                    run. Without this, `play` waits for an endpoint to answer
                    again, rebuilds, and resumes from the frame the device had
                    reached
  --recover-timeout S
                    how long to wait for that endpoint. Default 30
  --interactive     `play`: take keys while it plays. Space pauses and resumes,
                    [ and ] seek ten seconds, n starts the next track, p switches
                    between the bit-exact path and the processed one, q stops.
                    Pausing stops the device rather than feeding it silence, so
                    resuming lands on the sample it left
  --sink ID         which sink module: sink_wasapi (the default) or sink_asio.
                    ASIO needs a driver and takes the whole device, and is worth
                    asking for only for native DSD -- docs/plan.md section 5
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
                   arg == "show" ||
                   arg == "verify" || arg == "decode" || arg == "modules" ||
                   arg == "compare" || arg == "loudness" || arg == "claims") {
            out.command = arg;
        } else if (arg == "--file") {
            if (i + 1 < argc) {
                out.file = argv[++i];
                out.files.emplace_back(out.file);
            }
        } else if (arg == "--interactive") {
            out.interactive = true;
        } else if (arg == "--raw") {
            if (i + 1 < argc) {
                out.raw = argv[++i];
            }
        } else if (arg == "--decoder") {
            if (i + 1 < argc) {
                out.decoder_id = argv[++i];
                // **A short name means a demuxer**, because a demuxer is what
                // opening a file starts with: `--decoder ogg` is `demux_ogg`.
                // The kind is half of a module's name and abbreviating it away
                // would make `ogg` ambiguous between the container and the two
                // codecs, so anything else is spelled in full.
                if (out.decoder_id.rfind("demux_", 0) != 0 &&
                    out.decoder_id.rfind("codec_", 0) != 0) {
                    out.decoder_id = "demux_" + out.decoder_id;
                }
            }
        } else if (arg == "--target") {
            if (i + 1 < argc) {
                out.target_lufs = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--gain") {
            if (i + 1 < argc) {
                out.gain = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--dither-seed") {
            if (i + 1 < argc) {
                out.dither_seed =
                    static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            }
        } else if (arg == "--ring-periods") {
            value(out.ring_periods);
        } else if (arg == "--wait-timeout") {
            value(out.wait_timeout_ms);
        } else if (arg == "--no-recover") {
            out.recover = false;
        } else if (arg == "--recover-timeout") {
            value(out.recover_timeout);
        } else if (arg == "--block") {
            value(out.block);
        } else if (arg == "--dsp") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--dsp takes a stage name, or `list`\n");
                return false;
            }
            out.dsp.emplace_back(argv[++i]);
        } else if (arg == "--dither" && i + 1 < argc &&
                   std::string_view{argv[i + 1]} == "list") {
            list_algorithms();
            std::exit(0);
        } else if (arg == "--shape" && i + 1 < argc &&
                   std::string_view{argv[i + 1]} == "list") {
            list_algorithms();
            std::exit(0);
        } else if (arg == "--dither") {
            if (i + 1 >= argc || !mp::dither_kind_from_name(argv[++i], out.dither)) {
                std::fprintf(stderr,
                             "--dither takes none, rectangular, triangular, highpass "
                             "or gaussian\n");
                return false;
            }
        } else if (arg == "--shape") {
            if (i + 1 >= argc || !mp::noise_shaping_from_name(argv[++i], out.shaping)) {
                std::fprintf(stderr,
                             "--shape takes 0 to 9 for a binomial order, "
                             "shibata[:intensity], or one of:\n  %s\n"
                             "`--shape list` prints all of them with what they do.\n",
                             mp::named_shapers().c_str());
                return false;
            }
        } else if (arg == "--float") {
            out.float_source = true;
        } else if (arg == "--path") {
            if (i + 1 >= argc || !mp::path_policy_from_name(argv[++i], out.path)) {
                std::fprintf(stderr, "--path takes auto, exact or processed\n");
                return false;
            }
        } else if (arg == "--source") {
            if (i + 1 < argc) {
                out.source = argv[++i];
            }
        } else if (arg == "--rival") {
            if (i + 1 < argc) {
                out.rival_id = argv[++i];
                // The same rule as `--decoder`: a short name is a demuxer.
                if (out.rival_id != "none" && out.rival_id.rfind("demux_", 0) != 0 &&
                    out.rival_id.rfind("codec_", 0) != 0) {
                    out.rival_id = "demux_" + out.rival_id;
                }
            }
        } else if (arg == "--min-snr") {
            if (i + 1 < argc) {
                out.min_snr_db = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--min-lag-margin") {
            if (i + 1 < argc) {
                out.min_lag_margin_db = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--min-channel-margin") {
            if (i + 1 < argc) {
                out.min_channel_margin_db = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--vs-rival") {
            if (i + 1 < argc) {
                out.vs_rival_db = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--max-peak-ratio") {
            if (i + 1 < argc) {
                out.max_peak_ratio = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--band-tol") {
            if (i + 1 < argc) {
                out.band_tol_db = std::strtod(argv[++i], nullptr);
            }
        } else if (arg == "--band-limit") {
            value(out.band_limit_hz);
        } else if (arg == "--lag-window") {
            if (i + 1 < argc) {
                out.lag_window = std::atoi(argv[++i]);
            }
        } else if (arg == "--untrimmed") {
            // A raw stream carries no gapless metadata, so it is *correct* for
            // the decode to be longer than the audio that was encoded. Every
            // other check still applies, alignment included.
            out.exact_length = false;
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
            out.bits_given = true;
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
        } else if (arg == "--seek") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--seek needs a frame number\n");
                return false;
            }
            out.seek = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--device-name") {
            if (i + 1 < argc) {
                out.device_name = argv[++i];
            }
        } else if (arg == "--seconds") {
            out.seconds_given = true;
            std::uint32_t s = 0;
            value(s);
            out.seconds = s;
        } else if (arg == "--sink") {
            if (i + 1 < argc) {
                out.sink_id = argv[++i];
                // A short name means a sink, the way `--decoder ogg` means a
                // demuxer: `--sink asio` is `sink_asio`.
                if (out.sink_id.rfind("sink_", 0) != 0) {
                    out.sink_id = "sink_" + out.sink_id;
                }
            }
        } else if (arg == "--no-audio") {
            out.no_audio = true;
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

using mp::result_name;

mp::Format requested_format(const Options& options)
{
    mp::Format format;
    format.sample_rate = options.rate;
    format.channels = options.channels;
    format.channel_mask = 0;
    format.encoding = mp::Encoding::pcm;

    if (options.float_source) {
        format.sample_type = mp::SampleType::f32;
        return format;
    }

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
/// ASCII-lowered, which is all a substring match on a device name needs.
std::string lowered(std::string_view text)
{
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// The endpoint to open: an index, part of a name, or the default.
///
/// **An ambiguous name is refused rather than resolved.** In exclusive mode
/// opening the wrong endpoint is not a small mistake -- it takes that device
/// away from everything else on the machine -- so a name that matches two
/// devices ends the command instead of picking one.
bool device_id_for(const MpSinkVtbl& sink, const Options& options, std::string& out,
                   bool quiet = false)
{
    // Quiet is for polling: after the device was lost this runs four times a
    // second until one answers, and saying "no endpoint matches" a hundred
    // times is not more informative than saying it once.
    const auto complain = [quiet](const char* fmt, auto... rest) {
        if (!quiet) {
            std::fprintf(stderr, fmt, rest...);
        }
    };
    if (!options.device_name.empty()) {
        const std::string want = lowered(options.device_name);
        std::vector<std::pair<std::string, std::string>> hits; // name, id
        MpDeviceInfo info{};
        for (std::uint32_t index = 0;; ++index) {
            info.size = sizeof(info);
            const MpResult r = sink.enumerate(index, &info);
            if (r == MP_END) {
                break;
            }
            if (r != MP_OK) {
                complain("enumerate failed: %s\n", result_name(r));
                return false;
            }
            if (lowered(info.name).find(want) != std::string::npos) {
                hits.emplace_back(info.name, info.id);
            }
        }
        if (hits.empty()) {
            complain("no endpoint matches `%s`\n", options.device_name.c_str());
            return false;
        }
        if (hits.size() > 1) {
            complain("`%s` matches %zu endpoints:\n", options.device_name.c_str(),
                     hits.size());
            for (const auto& hit : hits) {
                complain("  %s\n", hit.first.c_str());
            }
            return false;
        }
        out = hits.front().second;
        return true;
    }

    if (options.device_index < 0) {
        out.clear();
        return true;
    }
    MpDeviceInfo info{};
    info.size = sizeof(info);
    const MpResult r = sink.enumerate(static_cast<std::uint32_t>(options.device_index), &info);
    if (r != MP_OK) {
        complain("no device with index %d\n", options.device_index);
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

/// Why nothing was accepted, said in terms of what was asked for.
///
/// "No candidate was accepted" is the same sentence whether the device refused
/// everything or the user asked for a graph that does not exist, and those are
/// very different things to be told.
void report_refusal(const mp::Negotiated& negotiated)
{
    switch (negotiated.policy) {
    case mp::PathPolicy::processed:
        std::fprintf(stderr,
                     "--path processed asks Path B for a format this device will take, and\n"
                     "it took none of the %zu offered: %s\n"
                     "Path B converts the sample type and applies a gain, and a\n"
                     "`--dsp resample:rate=N` stage changes the sample rate. Nothing here\n"
                     "changes the channel count, so a device that wants a different one\n"
                     "is still a refusal.\n",
                     negotiated.tried, result_name(negotiated.last_error));
        return;
    case mp::PathPolicy::automatic:
        std::fprintf(stderr,
                     "the device took none of the %zu formats offered, bit-exact or "
                     "converted: %s\n",
                     negotiated.tried, result_name(negotiated.last_error));
        return;
    case mp::PathPolicy::exact_only:
        std::fprintf(stderr,
                     "--path exact allows only a memcpy, and the device took none of the "
                     "%zu format%s offered: %s\n"
                     "Without --path, a container repack would also have been offered.\n",
                     negotiated.tried, negotiated.tried == 1 ? "" : "s",
                     result_name(negotiated.last_error));
        return;
    case mp::PathPolicy::bit_exact:
        break;
    }
    std::fprintf(stderr,
                 "no bit-exact candidate was accepted (%zu offered): %s\n"
                 "--path auto would also offer to convert, and say so when it did.\n",
                 negotiated.tried, result_name(negotiated.last_error));
}

int negotiate(const MpSinkVtbl& vtbl, const Options& options)
{
    const mp::Format source = requested_format(options);
    if (!mp::is_valid(source)) {
        std::fprintf(stderr, "that is not a format: %s\n", mp::describe(source).c_str());
        return 1;
    }

    std::string device_id;
    if (!device_id_for(vtbl, options, device_id)) {
        return 1;
    }
    mp::Sink sink = open_sink(vtbl, device_id, options.shared);
    if (!sink) {
        return 1;
    }

    std::printf("source     %s\n", mp::describe(source).c_str());
    std::printf("mode       %s\n", options.shared ? "shared" : "exclusive");
    std::printf("path       %s\n\n", mp::path_policy_name(options.path));

    const auto candidates = mp::build_candidates(source, options.path);
    std::size_t index = 0;
    for (const auto& candidate : candidates) {
        mp::Format accepted{};
        const MpResult r = sink.negotiate(candidate.format, accepted);
        const char* label = candidate.fidelity == mp::Fidelity::exact      ? "exact"
                            : candidate.fidelity == mp::Fidelity::repacked ? "repack"
                                                                          : "CONVERT";

        std::printf("%zu %-7s %-5s  %-44s ", ++index, label,
                    candidate.channel_mask_added ? "+mask" : "",
                    mp::describe(candidate.format).c_str());

        if (r != MP_OK) {
            std::printf("-> %s\n", result_name(r));
            continue;
        }

        const mp::Fidelity actual = mp::classify(source, accepted);
        if (!mp::allows(options.path, actual)) {
            // Either the device said yes and meant something else, or it meant
            // something this policy does not allow. Both are refusals here.
            std::printf("-> accepted %s -- %s, refusing\n", mp::describe(accepted).c_str(),
                        mp::is_bit_exact(actual) ? "not what --path allows" : "NOT bit-exact");
            continue;
        }

        std::uint32_t period = 0;
        sink.period_frames(period);
        std::printf("-> ACCEPTED, buffer %u frames (%.2f ms)\n", period,
                    1000.0 * period / accepted.sample_rate);
        return 0;
    }

    std::printf("\nnothing was accepted. %zu candidate%s offered.\n", candidates.size(),
                candidates.size() == 1 ? "" : "s");
    if (options.path == mp::PathPolicy::exact_only) {
        std::printf("--path exact offers only the source's own container. Without it, a\n"
                    "container repack would also have been tried.\n");
    }
    return 1;
}

/// Starts a graph, plays for as long as it was asked to, and reports.
///
/// A template because Path A and Path B are two classes rather than one class
/// with a branch -- which is §2's rule and is about the render thread, not about
/// this. From out here they are the same object: start it, watch it, stop it,
/// ask it how it went.
/// The keyboard, while something is playing.
///
/// Not a user interface -- this is a diagnostic tool and the shell is somebody
/// else's job -- but transport that cannot be exercised is transport nobody has
/// checked, and a person is a better test of "did the device notice" than any
/// assertion.
enum class KeyResult {
    carry_on,
    stop,
    /// Somebody asked for the other path. The graph cannot change which one it
    /// is -- that decision is made when it is built -- so this ends the run and
    /// the host builds the next one differently.
    switch_path,
};

template <typename Graph>
KeyResult handle_keys(Graph& graph, mp::Queue* queue, const mp::Format& source)
{
    while (_kbhit() != 0) {
        const int key = _getch();
        switch (key) {
        case ' ':
            if (graph.paused()) {
                graph.resume();
                std::printf("  resumed\n");
            } else {
                graph.pause();
                std::printf("  paused at %.2f s\n",
                            static_cast<double>(graph.position_frames()) /
                                source.sample_rate);
            }
            std::fflush(stdout);
            break;
        case '[':
        case ']': {
            const std::int64_t step =
                static_cast<std::int64_t>(source.sample_rate) * (key == ']' ? 10 : -10);
            const auto now = static_cast<std::int64_t>(graph.position_frames());
            const auto target = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now + step));
            if (graph.seek(target)) {
                std::printf("  %.2f s\n", static_cast<double>(target) / source.sample_rate);
            } else {
                std::printf("  this source cannot seek\n");
            }
            std::fflush(stdout);
            break;
        }
        case 'n':
            if (queue != nullptr) {
                queue->skip();
                std::printf("  next\n");
                std::fflush(stdout);
            }
            break;
        case 'p':
            if (queue != nullptr) {
                return KeyResult::switch_path;
            }
            std::printf("  a tone has no position, so there is nowhere to switch "
                        "back to\n");
            std::fflush(stdout);
            break;
        case 'q':
            return KeyResult::stop;
        default:
            break;
        }
    }
    return KeyResult::carry_on;
}

/// What one run of the graph came to.
///
/// More than an exit code, because the caller has to decide whether to build
/// the whole thing again. A file that ended and a device that was pulled out
/// both stop the graph, and the difference between them is this struct.
struct RunOutcome {
    /// The exit code this run would have earned on its own.
    int rc = 0;
    /// Why the graph stopped, when it stopped by itself.
    MpResult error = MP_OK;
    /// Which source frame the device had been given when it stopped, and so
    /// where a resumed run begins.
    std::uint64_t position = 0;
    /// The run ended because somebody asked for the other path.
    bool switch_path = false;
};

template <typename Graph>
RunOutcome run_graph(Graph& graph, mp::win::RenderThreadHooks& hooks, const mp::Format& wire,
                     unsigned limit_seconds, const Options* options = nullptr,
                     mp::Queue* queue = nullptr, const mp::Format* source = nullptr)
{

    const MpResult started = graph.start();
    if (started != MP_OK) {
        std::fprintf(stderr, "could not start: %s\n", result_name(started));
        // Not always fatal: a device can go between being negotiated with and
        // being started, and that is the same event as losing it later.
        return RunOutcome{1, started, graph.position_frames()};
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
    // A file with a known end plays to it; a tone is asked to stop.
    const bool timed = limit_seconds != 0;
    const auto until = began + std::chrono::seconds{limit_seconds};
    auto next_report = began + std::chrono::seconds{5};

    bool asked_to_switch = false;
    while ((!timed || std::chrono::steady_clock::now() < until) && graph.running()) {
        if (options != nullptr && options->interactive) {
            const KeyResult key =
                handle_keys(graph, queue, source != nullptr ? *source : wire);
            if (key != KeyResult::carry_on) {
                asked_to_switch = key == KeyResult::switch_path;
                break;
            }
        }
        // Short enough that a keypress does not sit waiting, long enough that
        // this is not a spin.
        std::this_thread::sleep_for(std::chrono::milliseconds{
            options != nullptr && options->interactive ? 20 : 100});
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
                           wire.sample_rate;
    std::printf("\nplayed     %llu frames (%.2f s)\n",
                static_cast<unsigned long long>(stats.frames_rendered), seconds);
    std::printf("underruns  %llu", static_cast<unsigned long long>(stats.underruns));
    if (stats.underruns != 0) {
        std::printf("  (%llu frames of silence)",
                    static_cast<unsigned long long>(stats.silent_frames));
    }
    std::printf("\ntimeouts   %llu\n", static_cast<unsigned long long>(stats.wait_timeouts));
    if (stats.tail_frames != 0) {
        // Said plainly, because it looks like a fault and is not one.
        std::printf("tail       %llu frames of padding: the file ended mid-period\n",
                    static_cast<unsigned long long>(stats.tail_frames));
    }

    const std::uint64_t position = graph.position_frames();
    const MpResult error = graph.error();
    if (error != MP_OK) {
        std::printf("stopped    %s\n", result_name(error));
        return RunOutcome{1, error, position, false};
    }
    return RunOutcome{stats.underruns == 0 ? 0 : 2, MP_OK, position, asked_to_switch};
}

#if MEDIAPERCH_HAS_LOUDNESS
/// Scans a whole file and says how loud it is.
///
/// The measurement a player needs *before* it plays: ReplayGain cannot be
/// applied in the same pass it is measured in, because a track's integrated
/// loudness is not known until the track has finished. So this is the pass that
/// finds the number, and `--dsp replaygain:gain_db=` is the pass that uses it.
///
/// It is the same meter the stage runs, which is the point of it being a
/// library: a number this prints and a number the stage reports cannot disagree.
int loudness(mp::ISource& input, const Options& options)
{
    const mp::Format source = input.format();
    const mp::Format bus = mp::dsp_bus_format(source);
    mp::Converter to_bus{source, bus, mp::ConvertConfig{}};
    if (!to_bus.possible()) {
        std::fprintf(stderr, "cannot read %s as samples\n",
                     mp::describe(source).c_str());
        return 1;
    }

    mp::loudness::Meter meter;
    std::string why;
    if (!meter.configure(source.sample_rate, source.channels, source.channel_mask, why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }

    constexpr std::uint32_t k_chunk = 16384;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(k_chunk) *
                                  mp::frame_bytes(source));
    std::vector<double> interleaved(static_cast<std::size_t>(k_chunk) * source.channels);
    std::vector<double> planar(interleaved.size());
    std::vector<const double*> planes(source.channels);

    std::uint64_t frames_read = 0;
    for (;;) {
        const std::size_t got = input.read(raw.data(), raw.size());
        const std::size_t frames = got / mp::frame_bytes(source);
        if (frames == 0) {
            break;
        }
        to_bus.run(raw.data(), interleaved.data(), frames);
        for (std::uint32_t c = 0; c < source.channels; ++c) {
            double* plane = planar.data() + static_cast<std::size_t>(c) * frames;
            for (std::size_t n = 0; n < frames; ++n) {
                plane[n] = interleaved[n * source.channels + c];
            }
            planes[c] = plane;
        }
        meter.add(planes.data(), static_cast<std::uint32_t>(frames));
        frames_read += frames;
    }

    std::printf("file       %s\n", options.file.c_str());
    std::printf("source     %s, %llu frames (%.2f s)\n", mp::describe(source).c_str(),
                static_cast<unsigned long long>(frames_read),
                static_cast<double>(frames_read) / source.sample_rate);
    const double measured = meter.integrated_lufs();
    if (measured <= mp::loudness::Meter::silence() / 2.0) {
        std::printf("loudness   silence: nothing passed the -70 LUFS gate\n");
        return 0;
    }
    std::printf("loudness   %.2f LUFS integrated, over %llu blocks\n", measured,
                static_cast<unsigned long long>(meter.blocks()));
    std::printf("peak       %.2f dBFS (sample peak; true peak is not measured)\n",
                meter.sample_peak_db());
    const double gain = meter.replay_gain_db(options.target_lufs);
    std::printf("replaygain %+.2f dB to reach %.1f LUFS\n", gain, options.target_lufs);
    const double after = meter.sample_peak() * std::pow(10.0, gain / 20.0);
    if (after > 1.0) {
        std::printf("           applying it would put the peak at %+.2f dBFS, so the\n"
                    "           stage will hold it to %+.2f dB unless told not to\n",
                    20.0 * std::log10(after), -20.0 * std::log10(meter.sample_peak()));
    }
    std::printf("\n  --dsp replaygain:gain_db=%.2f,peak=%.6f\n", gain,
                meter.sample_peak());
    return 0;
}
#endif

/// A module id with its kind trimmed off, for a column heading: `demux_ffmpeg`
/// is "ffmpeg" to somebody reading a table.
const char* rival_label(const std::string& id)
{
    if (id.rfind("demux_", 0) == 0 || id.rfind("codec_", 0) == 0) {
        return id.c_str() + id.find('_') + 1;
    }
    return id.c_str();
}

/// Opens a file the way the engine does: the container first, then the v1
/// decoders that are still being moved across.
///
/// **Every command that reads a file goes through this**, and that is the point
/// rather than a convenience. `decode` prints a hash, `compare` measures against
/// the source, `verify` sends the bytes to a device and reads them back, and
/// `loudness` meters them -- four answers about one file that would be worth
/// nothing if they were answers about four different ways of opening it.
std::unique_ptr<mp::ISource> open_file(mp::win::EngineHost& host, const std::string& path,
                                       std::string_view prefer, const char* what)
{
    std::string opened_by;
    std::string why;
    auto source = host.open_source(path, prefer, opened_by, why);
    if (!source) {
        std::fprintf(stderr, "cannot %s %s: %s\n", what, path.c_str(),
                     why.empty() ? "nothing here reads it" : why.c_str());
        return nullptr;
    }
    return source;
}

/// The two numbers that decide how much slack the decode thread has.
mp::PassthroughConfig buffering(const Options& options)
{
    mp::PassthroughConfig config;
    config.ring_periods = std::max(2u, options.ring_periods);
    config.wait_timeout_ms = options.wait_timeout_ms;
    return config;
}

/// One `--dsp` argument: `name` or `name:key=value,key=value`.
///
/// The name may leave off the `dsp_` the module calls itself by, the same way
/// `--decoder mp3` may, because nobody types a prefix twice.
bool add_dsp_stage(const mp::win::ModuleRegistry& registry, const std::string& spec,
                   mp::DspChain& chain, std::string& why)
{
    const std::size_t colon = spec.find(':');
    std::string id = spec.substr(0, colon);
    if (id.rfind("dsp_", 0) != 0) {
        id = "dsp_" + id;
    }
    const MpDspVtbl* vtbl = registry.dsp(id);
    if (vtbl == nullptr) {
        why = "no DSP module called " + id + " is loaded; `--dsp list` says which are";
        return false;
    }
    chain.add(*vtbl, id);
    mp::DspStage& stage = chain.at(chain.size() - 1);
    if (!stage.open()) {
        why = id + " would not open";
        return false;
    }
    if (colon == std::string::npos) {
        return true;
    }

    for (std::size_t at = colon + 1; at <= spec.size();) {
        const std::size_t comma = std::min(spec.find(',', at), spec.size());
        const std::string setting = spec.substr(at, comma - at);
        at = comma + 1;
        if (setting.empty()) {
            continue;
        }
        const std::size_t equals = setting.find('=');
        if (equals == std::string::npos) {
            why = id + ": `" + setting + "` is not key=value";
            return false;
        }
        const std::string key = setting.substr(0, equals);
        if (stage.set(key, setting.substr(equals + 1)) != MP_OK) {
            why = id + " would not take `" + setting + "`; `--dsp list` says what it takes";
            return false;
        }
    }
    return true;
}

/// `--dsp list`: every stage that is loaded, and every knob it has.
int list_dsp_modules(const mp::win::ModuleRegistry& registry)
{
    const auto found = registry.dsps();
    if (found.empty()) {
        std::printf("no DSP modules beside the executable\n");
        return 1;
    }
    for (const MpModuleDesc* desc : found) {
        std::printf("%-16s %s\n", desc->id, desc->name);
        const MpDspVtbl* vtbl = registry.dsp(desc->id);
        if (vtbl == nullptr) {
            std::printf("    (this module does not offer a DSP this host can use)\n\n");
            continue;
        }
        mp::DspStage stage{*vtbl, desc->id};
        if (!stage.open()) {
            std::printf("    (it would not open)\n\n");
            continue;
        }
        for (const std::string& line : stage.describe()) {
            // key \t current \t what it does, which is the module's own words.
            const std::size_t first = line.find('\t');
            const std::size_t second = line.find('\t', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                std::printf("    %s\n", line.c_str());
                continue;
            }
            std::printf("    %-10s %-12s %s\n", line.substr(0, first).c_str(),
                        line.substr(first + 1, second - first - 1).c_str(),
                        line.substr(second + 1).c_str());
        }
        std::printf("\n");
    }
    std::printf("--dsp NAME, or --dsp NAME:key=value,key=value. Repeat it for a chain;\n"
                "they run in the order given.\n");
    return 0;
}

/// The files on the command line, opened one at a time.
///
/// Lazy on purpose: a playlist of five hundred entries is five hundred open
/// decoders if the queue is handed a list, and none of them are needed until
/// the one before finishes. `IPlaylist` exists so that this decision lives
/// here, in the host, and not in the core.
class FilePlaylist final : public mp::IPlaylist {
public:
    FilePlaylist(mp::win::EngineHost& host, const Options& options)
        : host_(&host), options_(&options)
    {
    }

    mp::ISource* at(std::size_t index) override
    {
        if (index >= options_->files.size()) {
            return nullptr;
        }
        opened_.resize(std::max(opened_.size(), index + 1));
        if (opened_[index]) {
            return opened_[index].get();
        }
        const std::string& path = options_->files[index];
        std::string opened_by;
        std::string why;
        auto source = host_->open_source(path, options_->decoder_id, opened_by, why);
        if (!source) {
            // Recorded rather than fatal: a playlist that silently plays four of
            // its five entries is worse than one that says which it skipped.
            std::fprintf(stderr, "skipping %s: %s\n", path.c_str(),
                         why.empty() ? "no module recognised it" : why.c_str());
            return nullptr;
        }
        names_.resize(std::max(names_.size(), index + 1));
        names_[index] = opened_by;
        opened_[index] = std::move(source);
        return opened_[index].get();
    }

    [[nodiscard]] const std::string& decoder_name(std::size_t index) const
    {
        static const std::string none;
        return index < names_.size() ? names_[index] : none;
    }

private:
    mp::win::EngineHost* host_;
    const Options* options_;
    std::vector<std::unique_ptr<mp::ISource>> opened_;
    std::vector<std::string> names_;
};

/// One run: everything from the source to the device, played once.
///
/// A run ends when the audio does -- or when the next track is in a different
/// format, which needs the device reopened and is therefore a new run with an
/// audible gap between them. `play` is the loop over runs.
RunOutcome play_run(const MpSinkVtbl& vtbl, const mp::win::ModuleRegistry& registry,
                    const Options& options, mp::ISource& queued, mp::Queue* queue,
                    const std::string& label);

/// Whether there is an endpoint there that will open.
///
/// Opening it is the test rather than enumerating it: an endpoint can be listed
/// while its driver is still coming back, and a device that will not open is
/// not one to rebuild on.
bool device_ready(const MpSinkVtbl& vtbl, const Options& options)
{
    std::string id;
    if (!device_id_for(vtbl, options, id, /*quiet=*/true)) {
        return false;
    }
    MpSink* handle = nullptr;
    if (vtbl.open(id.empty() ? nullptr : id.c_str(),
                  options.shared ? MP_SHARE_SHARED : MP_SHARE_EXCLUSIVE, &handle) != MP_OK) {
        return false;
    }
    vtbl.close(handle);
    return true;
}

/// The device went away in the middle of a run.
///
/// Not necessarily a fault, and usually not one: a USB DAC that was unplugged,
/// a driver that restarted, a default endpoint that changed underneath us,
/// somebody altering the sample rate in the control panel. WASAPI answers every
/// one of those with `AUDCLNT_E_DEVICE_INVALIDATED`, there is no way to argue
/// with it, and the only thing to be done is to build the whole path again on
/// whatever endpoint is there now.
///
/// **What was lost is smaller than it looks.** The decoder is still open, the
/// file has not moved, and the chain rebuilds from the same `--dsp` arguments,
/// so the only casualties are the device and the frames the ring was holding
/// for it. That is exactly why the resume point is what the *device* was given
/// and not how far the decoder had read: the ring's contents were never played
/// and never will be, and resuming from the decoder would skip a ring's worth
/// of audio without mentioning it.
bool recover_device(const MpSinkVtbl& vtbl, const Options& options,
                    const RunOutcome& run, mp::Queue* queue)
{
    if (!options.recover) {
        return false;
    }
    if (queue == nullptr) {
        // A tone has no position to come back to, and playing another
        // `--seconds` of it would be a different run wearing the same name.
        std::printf("\ndevice     lost. There is nothing to resume: a tone has no "
                    "position\n           and --file is what gives a run one.\n");
        return false;
    }
    std::printf("\ndevice     lost -- unplugged, restarted, or reconfigured under us.\n");
    if (!device_ready(vtbl, options)) {
        std::printf("waiting    for an endpoint to answer (up to %u s)\n",
                    options.recover_timeout);
        std::fflush(stdout);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{options.recover_timeout};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            if (device_ready(vtbl, options)) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "no endpoint came back within %u s\n",
                             options.recover_timeout);
                return false;
            }
        }
    }
    if (!queue->seek(run.position)) {
        std::fprintf(stderr, "the device came back but the source cannot seek, so "
                             "there is\nnowhere to resume from\n");
        return false;
    }
    const double seconds =
        static_cast<double>(run.position) / queue->format().sample_rate;
    std::printf("resuming   at frame %llu (%.2f s). Up to one device buffer had "
                "been handed\n           over and not yet played, and that much is "
                "not played twice.\n\n",
                static_cast<unsigned long long>(run.position), seconds);
    return true;
}

/// Somebody asked for the other path, in the middle of a track.
///
/// **The device has to stop.** Exclusive mode has no way to change what feeds a
/// running stream, and a graph decides which path it is when it is built, so
/// the only place a path can change is between one graph and the next. That
/// gap is real: it is the device stopping, the ring refilling, and the device
/// starting again.
///
/// What is *not* real is a glitch. The next run begins on the frame the device
/// stopped on -- not on the one the decoder had reached, which is a ring ahead
/// -- so no sample is played twice and none is skipped. Continuity of the audio
/// across a rebuild is the thing a rebuild point can honestly promise, and it
/// is the same promise, kept by the same code, as coming back from a device
/// that was pulled out.
bool switch_path(Options& current, const RunOutcome& run, mp::Queue& queue)
{
    // Whatever is decided below, the next run starts on the frame this one
    // stopped on. That is true even when the answer is "no": a run that was
    // interrupted and resumed on the same path must still resume, not restart a
    // ring's worth ahead of where anybody had got to.
    if (!queue.seek(run.position)) {
        std::fprintf(stderr, "this source cannot seek, so it cannot resume on "
                             "another path\n");
        return false;
    }
    if (!current.dsp.empty()) {
        std::printf("\npath       there is a --dsp chain in the path, and a chain "
                    "exists in order to\n           change the samples. There is "
                    "nothing for a policy to decide.\n\n");
        return true; // carry on where we were, on the same path
    }
    const mp::PathPolicy next = current.path == mp::PathPolicy::processed
                                    ? mp::PathPolicy::bit_exact
                                    : mp::PathPolicy::processed;
    current.path = next;
    std::printf("\npath       switching to %s at frame %llu (%.2f s). The device "
                "stops and\n           starts; the audio carries on from the sample "
                "it stopped on.\n\n",
                mp::path_policy_name(next),
                static_cast<unsigned long long>(run.position),
                static_cast<double>(run.position) / queue.format().sample_rate);
    return true;
}

/// `show`: one file, one window, and §8 deciding when each frame goes up.
///
/// **One process, one window**, which is §9.7.1's MP_SURFACE_WINDOW case and
/// the one a tool has. The engine deliberately does not do this: a headless
/// engine that creates windows is not headless, and the daemon's picture will
/// cross to a shell as a DirectComposition surface instead. This is
/// mediaperch-probe, which is one program looking at one file, and it is how
/// the video path gets looked at before there is a shell to look through.
///
/// The shape is the whole of what has been built: one demuxer, a router
/// feeding both halves, the audio graph that owns the master clock, and a
/// display loop that reads that clock and pumps the video graph against it.
int show(const MpSinkVtbl& sink_vtbl, const mp::win::ModuleRegistry& registry,
         const Options& options)
{
    if (options.files.empty()) {
        std::fprintf(stderr, "show needs a file: --file <path>\n");
        return 1;
    }
    const std::string& path = options.files.front();

    // A demuxer that claims the container. Best first, and the next one when
    // the best declines what is inside -- the same rule `open_source` follows.
    mp::Demux demux;
    const MpDemuxVtbl* demux_vtbl = nullptr;
    for (const auto& choice : registry.demuxers_for(path, options.decoder_id)) {
        if (demux.open(*choice.vtbl, path.c_str()) == MP_OK) {
            demux_vtbl = choice.vtbl;
            std::printf("container  %s\n", choice.desc->id);
            break;
        }
        demux.close();
    }
    if (demux_vtbl == nullptr) {
        std::fprintf(stderr, "nothing here reads %s\n", path.c_str());
        return 1;
    }

    std::uint32_t audio_stream = 0;
    std::uint32_t video_stream = 0;
    bool have_audio = false;
    bool have_video = false;
    MpStreamInfo audio_info{};
    MpStreamInfo video_info{};
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        MpStreamInfo info{};
        if (!demux.stream_info(i, info)) {
            continue;
        }
        if (info.kind == MP_STREAM_AUDIO && !have_audio) {
            audio_stream = i;
            audio_info = info;
            have_audio = true;
        }
        if (info.kind == MP_STREAM_VIDEO && !have_video) {
            video_stream = i;
            video_info = info;
            have_video = true;
        }
    }
    if (!have_video) {
        std::fprintf(stderr, "%s has no video in it -- `play` is the command for that\n",
                     path.c_str());
        return 1;
    }

    MpVideoInfo picture{};
    picture.size = sizeof(picture);
    if (!demux.video_info(video_stream, picture) || picture.width == 0) {
        std::fprintf(stderr, "the container would not describe its video stream\n");
        return 1;
    }

    // **One demuxer, two consumers.** §4: one file has one position, so the
    // audio and the video come out of the same read and a seek moves both.
    std::vector<std::uint32_t> selected;
    if (have_audio) {
        selected.push_back(audio_stream);
    }
    selected.push_back(video_stream);
    if (demux.select_streams(selected) != MP_OK) {
        std::fprintf(stderr, "the container would not serve those streams together\n");
        return 1;
    }
    mp::PacketRouter router{demux, selected};

    // ---- the window, before the presenter, because the presenter wants an HWND
    mp::win::VideoWindow window;
    std::string why;
    if (!window.open(path, picture.display_width != 0 ? picture.display_width : picture.width,
                     picture.display_height != 0 ? picture.display_height : picture.height,
                     why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }

    const MpVideoVtbl* video_vtbl = registry.video();
    if (video_vtbl == nullptr) {
        std::fprintf(stderr, "no presenter module is loaded\n");
        return 1;
    }
    mp::Presenter presenter;
    if (presenter.open(*video_vtbl, window.handle()) != MP_OK ||
        presenter.configure(picture) != MP_OK) {
        std::fprintf(stderr, "the presenter would not take that picture\n");
        return 1;
    }

    // §9.8.1: the decoder is handed the presenter's device rather than making
    // one, so a hardware decoder's textures are ones this presenter can sample.
    MpGraphicsDevice device{};
    const bool have_device = presenter.get_device(device) == MP_OK;

    std::vector<std::uint8_t> config;
    (void)demux.stream_config(video_stream, config);
    const MpVideoCodecVtbl* codec_vtbl = registry.video_codec_for(
        video_info.codec, have_device ? MP_GRAPHICS_D3D11 : MP_GRAPHICS_NONE,
        config.empty() ? nullptr : config.data(), static_cast<std::uint32_t>(config.size()));
    if (codec_vtbl == nullptr) {
        std::fprintf(stderr, "nothing here decodes that video codec\n");
        return 1;
    }
    mp::VideoDecoder decoder;
    if (decoder.open(*codec_vtbl, video_info.codec, have_device ? &device : nullptr,
                     config.empty() ? nullptr : config.data(),
                     static_cast<std::uint32_t>(config.size())) != MP_OK) {
        std::fprintf(stderr, "the video decoder would not open this stream\n");
        return 1;
    }
    std::printf("picture    %ux%u", picture.width, picture.height);
    if (picture.fps_den != 0) {
        std::printf(" at %.3f fps", static_cast<double>(picture.fps_num) / picture.fps_den);
    }
    std::printf("\n");

    mp::IPacketFeed* video_feed = router.feed(video_stream);
    if (video_feed == nullptr) {
        std::fprintf(stderr, "the router has no feed for the video stream\n");
        return 1;
    }
    mp::VideoGraph video_graph{*video_feed, decoder, presenter, picture};

    // ---- the audio half, which is where the master clock comes from
    mp::PacketSource audio_source;
    std::unique_ptr<mp::PassthroughGraph> exact;
    std::unique_ptr<mp::ProcessedGraph> processed;
    std::unique_ptr<mp::IAudioClockSource> audio_clock;
    mp::Sink sink;

    if (have_audio && !options.no_audio) {
        const mp::PacketSource::FindCodec find = [&registry](MpCodec codec,
                                                             const std::uint8_t* blob,
                                                             std::uint32_t blob_bytes) {
            return registry.codec_for(codec, blob, blob_bytes);
        };
        if (!audio_source.open(router, audio_stream, find, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
        std::string device_id;
        if (!device_id_for(sink_vtbl, options, device_id)) {
            return 1;
        }
        sink = open_sink(sink_vtbl, device_id, options.shared);
        if (!sink) {
            return 1;
        }
        const auto negotiated =
            mp::negotiate_best(sink, audio_source.format(), options.path);
        if (!negotiated.ok) {
            report_refusal(negotiated);
            return 1;
        }
        std::uint32_t period = 0;
        if (sink.period_frames(period) != MP_OK || period == 0) {
            std::fprintf(stderr, "the device did not report a buffer size\n");
            return 1;
        }
        std::printf("audio      %s\n", mp::describe(negotiated.accepted).c_str());
        if (mp::use_processed(options.path, negotiated.fidelity, options.gain != 1.0)) {
            processed = std::make_unique<mp::ProcessedGraph>(
                audio_source, sink, negotiated.accepted, period, mp::ConvertConfig{});
            if (processed->start() != MP_OK) {
                std::fprintf(stderr, "the audio graph would not start\n");
                return 1;
            }
            audio_clock =
                std::make_unique<mp::GraphClock<mp::ProcessedGraph>>(*processed);
        } else {
            exact = std::make_unique<mp::PassthroughGraph>(
                audio_source, sink, negotiated.accepted, period, negotiated.fidelity);
            if (exact->start() != MP_OK) {
                std::fprintf(stderr, "the audio graph would not start\n");
                return 1;
            }
            audio_clock = std::make_unique<mp::GraphClock<mp::PassthroughGraph>>(*exact);
        }
    }

    if (audio_clock == nullptr) {
        // **There is no master clock, and this says so rather than pretending.**
        // §8's clock is the audio device: a crystal that is actually producing
        // the sound somebody is listening to. A file with no audio track has
        // none, and neither does a run that was told not to open one. The
        // picture still has to go up at some rate, so the performance counter
        // stands in -- and the line below is there because a person watching
        // needs to know which of the two they are looking at.
        std::printf("clock      the wall clock -- %s, so there is nothing to follow\n",
                    have_audio ? "--no-audio was asked for" : "this file has no audio");
        auto wall = std::make_unique<mp::win::WallClock>(48000);
        wall->start();
        audio_clock = std::move(wall);
    }

    // ---- the display loop
    std::unique_ptr<mp::win::VBlankClock> vblank = mp::win::VBlankClock::open(window.handle());
    mp::win::TickClock ticks;
    mp::IFrameClock* frames = vblank != nullptr ? static_cast<mp::IFrameClock*>(vblank.get())
                                                : static_cast<mp::IFrameClock*>(&ticks);
    if (vblank != nullptr && vblank->refresh_hz() > 0.0) {
        std::printf("display    %.0f Hz, paced on its vertical blank\n",
                    vblank->refresh_hz());
    } else {
        std::printf("display    no output to wait on; pacing on a timer\n");
    }
    std::printf("\nclose the window to stop.\n");
    std::fflush(stdout);

    mp::DisplayLoop loop{video_graph, *audio_clock, *frames};
    std::atomic<bool> done{false};
    // **The loop is not on this thread**, because `WaitForVBlank` blocks for a
    // whole refresh and a message queue nobody drains for sixteen milliseconds
    // is a window Windows calls unresponsive.
    std::thread painter{[&] {
        loop.run();
        done.store(true, std::memory_order_release);
    }};

    while (!done.load(std::memory_order_acquire) && window.pump_messages()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{4});
    }
    if (vblank != nullptr) {
        vblank->cancel();
    }
    ticks.cancel();
    painter.join();

    if (exact) {
        exact->stop();
    }
    if (processed) {
        processed->stop();
    }

    const mp::VideoGraph::Stats stats = video_graph.stats();
    const mp::DisplayLoop::Stats turns = loop.stats();
    std::printf("\nframes     %llu shown, %llu dropped, %llu decoded\n",
                static_cast<unsigned long long>(stats.shown),
                static_cast<unsigned long long>(stats.dropped),
                static_cast<unsigned long long>(stats.decoded));
    std::printf("first      %.1f ms late -- the clock ran while it was being decoded\n",
                stats.first_late_seconds * -1000.0);
    std::printf("worst      %.1f ms late after that, against a %.1f ms refresh\n",
                stats.worst_late_seconds * -1000.0,
                vblank != nullptr && vblank->refresh_hz() > 0.0
                    ? 1000.0 / vblank->refresh_hz()
                    : 0.0);
    std::printf("turns      %llu, %llu of them with no clock to read\n",
                static_cast<unsigned long long>(turns.turns),
                static_cast<unsigned long long>(turns.without_clock));
    if (video_graph.error() != MP_OK) {
        std::fprintf(stderr, "the video stopped: %s\n",
                     mp::result_name(video_graph.error()));
        return 1;
    }
    return 0;
}

int play(const MpSinkVtbl& vtbl, const mp::win::ModuleRegistry& registry,
         const Options& options)
{
    if (options.files.empty()) {
        // The tone generator: no playlist, no queue, nothing to join.
        if (options.float_source) {
            std::fprintf(stderr,
                         "--float is for `negotiate`: the tone generator writes integers.\n");
            return 1;
        }
        const mp::Format wanted = requested_format(options);
        if (!mp::is_valid(wanted)) {
            std::fprintf(stderr, "that is not a format: %s\n",
                         mp::describe(wanted).c_str());
            return 1;
        }
        mp::SineSource tone{wanted, options.hz, options.amplitude};
        const RunOutcome run = play_run(vtbl, registry, options, tone, nullptr, "");
        if (run.error == MP_ERR_DEVICE_LOST) {
            (void)recover_device(vtbl, options, run, nullptr);
        }
        return run.rc;
    }

    // The playlist opens files the way the engine does. `play` here is the
    // probe's own device path, and it and `mediaperchd` disagreeing about which
    // module reads a file would make every measurement taken with one of them a
    // measurement about the other.
    mp::LogRing log;
    (void)log.listen([&options](const std::string& line) {
        if (options.verbose) {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    });
    mp::win::EngineHost host{registry, log};
    FilePlaylist playlist{host, options};
    std::size_t start = 0;
    for (;;) {
        mp::Queue queue{playlist, start};
        std::string why;
        if (!queue.open(why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
        if (options.seek != 0 && start == 0 && !queue.seek(options.seek)) {
            std::fprintf(stderr, "could not seek to frame %llu\n",
                         static_cast<unsigned long long>(options.seek));
            return 1;
        }
        // One queue, as many devices as it takes. The inner loop is the whole
        // of device-loss recovery: the source outlives the device, so losing
        // one is a rebuild and not an ending.
        RunOutcome run;
        // The options a run is built from, which a path switch edits. Copied so
        // that what the command line said stays what the command line said.
        Options current = options;
        bool just_switched = false;
        for (;;) {
            run = play_run(vtbl, registry, current, queue, &queue,
                           playlist.decoder_name(queue.index()));
            if (run.switch_path) {
                if (!switch_path(current, run, queue)) {
                    break;
                }
                just_switched = true;
                continue;
            }
            if (just_switched && run.rc == 1 && run.error == MP_OK) {
                // The other path was refused before anything played -- almost
                // always bit-exact on a device that will not take the file's
                // own format. Going back is better than ending a track over a
                // key somebody pressed to see what it did. The queue is still
                // where the switch left it, so there is nothing to seek.
                current.path = options.path;
                just_switched = false;
                std::printf("path       refused, so this is still %s\n\n",
                            mp::path_policy_name(current.path));
                continue;
            }
            just_switched = false;
            if (run.error != MP_ERR_DEVICE_LOST ||
                !recover_device(vtbl, current, run, &queue)) {
                break;
            }
        }
        if (run.rc != 0 || queue.stopped() != mp::QueueStop::format_change) {
            return run.rc;
        }
        // The one join a queue will not make. Saying so is the point: this is
        // the gap, it is the device's and not the player's, and it is where it
        // is because exclusive mode cannot change format without stopping.
        start = queue.index() + 1;
        std::printf("\nnext       %s is %s, which this device cannot take without\n"
                    "           being reopened. That is the gap, and it is not ours.\n\n",
                    options.files[start].c_str(),
                    mp::describe(queue.next_format()).c_str());
    }
}

RunOutcome play_run(const MpSinkVtbl& vtbl, const mp::win::ModuleRegistry& registry,
                    const Options& options, mp::ISource& queued, mp::Queue* queue,
                    const std::string& label)
{
    mp::ISource* source = &queued;
    const std::string& decoder_name = label;

    const mp::Format source_format = source->format();

    mp::DspChain chain;
    std::string why;
    for (const std::string& spec : options.dsp) {
        if (!add_dsp_stage(registry, spec, chain, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return RunOutcome{1};
        }
    }

    std::string device_id;
    if (!device_id_for(vtbl, options, device_id)) {
        return RunOutcome{1};
    }
    mp::Sink sink = open_sink(vtbl, device_id, options.shared);
    if (!sink) {
        return RunOutcome{1};
    }

    // What the device is asked for. A chain that resamples or remixes changes
    // what has to reach the device, so the rate and the channels come from its
    // output -- but the sample type stays the source's, because the f64 bus is
    // this program's business and offering the device f64 first would say the
    // file was something it was not.
    mp::Format offered = source_format;
    mp::PathPolicy policy = options.path;
    if (!chain.empty()) {
        // Sized provisionally: the device has not said how big a period is yet,
        // and all that is wanted here is the format that comes out the far end.
        if (!chain.configure(mp::dsp_bus_format(source_format), 4096, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return RunOutcome{1};
        }
        offered.sample_rate = chain.output_format().sample_rate;
        offered.channels = chain.output_format().channels;
        offered.channel_mask = chain.output_format().channel_mask;
        // A stage exists in order to change the samples. There is nothing left
        // for a policy to decide, and pretending otherwise would end in a
        // bit-exact claim over audio that had been through a filter.
        policy = mp::PathPolicy::processed;
    }

    const auto negotiated = mp::negotiate_best(sink, offered, policy);
    if (!negotiated.ok) {
        report_refusal(negotiated);
        return RunOutcome{1};
    }

    std::uint32_t period = 0;
    if (sink.period_frames(period) != MP_OK || period == 0) {
        std::fprintf(stderr, "the device did not report a buffer size\n");
        return RunOutcome{1};
    }

    if (!chain.empty() && !chain.configure(mp::dsp_bus_format(source_format), period, why)) {
        // Same chain, now sized for the block the graph will actually feed it.
        std::fprintf(stderr, "%s\n", why.c_str());
        return RunOutcome{1};
    }

    std::printf("device     %s\n", device_id.empty() ? "(default endpoint)" : device_id.c_str());
    std::printf("mode       %s\n", options.shared ? "shared" : "exclusive");
    std::printf("format     %s\n", mp::describe(negotiated.accepted).c_str());
    // A gain changes the samples whatever the formats say; a chain does too,
    // and is already the reason `policy` was forced above.
    const bool processing =
        mp::use_processed(policy, negotiated.fidelity, options.gain != 1.0);
    std::printf("path       %s%s  [--path %s]\n",
                processing                                    ? "PROCESSED -- the samples are changed"
                : negotiated.fidelity == mp::Fidelity::exact  ? "passthrough, memcpy"
                                                              : "passthrough, container repack",
                negotiated.channel_mask_added ? " (extensible form)" : "",
                mp::path_policy_name(negotiated.policy));
    if (processing) {
        // Loud, because §6.3 says it has to be. A converted stream is not the
        // thing this program is for, and a person who did not mean to ask for
        // one should be able to see that they did.
        std::printf("           source %s, gain %.4f, dither %s",
                    mp::describe(source_format).c_str(), options.gain,
                    mp::dither_kind_name(options.dither));
        std::printf(", shaping %s",
                    mp::noise_shaping_describe(options.shaping,
                                               negotiated.accepted.sample_rate)
                        .c_str());
        std::printf("\n");
        if (!chain.empty()) {
            std::printf("           chain ");
            for (std::size_t i = 0; i < chain.size(); ++i) {
                std::printf("%s%s", i == 0 ? "" : " -> ", chain.at(i).name().c_str());
            }
            std::printf(" on an f64 bus, %s\n",
                        mp::describe(chain.output_format()).c_str());
            if (const std::uint32_t delay = chain.latency_frames(); delay != 0) {
                // What is audible is this far behind what `position` says. Said
                // out loud because a player that shifts its own audio should say
                // by how much -- and because anything summing two chains into
                // one bus has to delay the shorter one by exactly this.
                std::printf("           latency %u frames (%.2f ms) the chain adds\n",
                            delay,
                            1000.0 * delay / chain.output_format().sample_rate);
            }
            if (options.path != mp::PathPolicy::processed) {
                std::printf("           --dsp implied --path processed\n");
            }
        }
    }
    std::printf("buffer     %u frames (%.2f ms)\n", period,
                1000.0 * period / negotiated.accepted.sample_rate);

    unsigned limit = options.seconds;
    if (queue != nullptr) {
        const std::uint64_t length = source->length_frames();
        std::printf("file       %s%s\n", options.files[queue->index()].c_str(),
                    options.files.size() > 1 ? "  (and the ones after it, gaplessly)"
                                             : "");
        std::printf("decoder    %s%s\n", decoder_name.c_str(),
                    options.decoder_id.empty() ? "" : "  [forced]");
        std::printf("source     %s", mp::describe(source_format).c_str());
        if (length != 0) {
            std::printf(", %llu frames (%.2f s)", static_cast<unsigned long long>(length),
                        static_cast<double>(length) / source_format.sample_rate);
        }
        std::printf("\n\n");
        // The file has an end, so the run does. `--seconds` still caps it for
        // somebody who only wants to hear the beginning of an hour-long file.
        limit = options.seconds_given ? options.seconds : 0;
    } else {
        std::printf("tone       %.1f Hz at %.3f of full scale (%.1f dBFS) for %u s\n\n",
                    options.hz, options.amplitude,
                    options.amplitude > 0.0 ? 20.0 * std::log10(options.amplitude) : -144.0,
                    options.seconds);

        // How much of the container this tone actually uses. A quiet tone is
        // safe for headphones and proves correspondingly less about a 32-bit
        // path, and saying so is cheaper than someone assuming otherwise. The
        // bit-exactness proof is the capture test, not this.
        const std::uint32_t container_bits =
            8 * mp::container_bytes(negotiated.accepted.sample_type);
        const double peak = options.amplitude * (std::pow(2.0, container_bits - 1) - 1.0);
        const auto used = peak >= 1.0 ? static_cast<int>(std::floor(std::log2(peak))) + 2 : 0;
        std::printf("exercises  %d of %u bits\n\n", used, container_bits);
    }

    mp::win::RenderThreadHooks hooks;
    if (processing) {
        mp::ConvertConfig conversion;
        conversion.gain = options.gain;
        conversion.dither = options.dither;
        conversion.shaping = options.shaping;
        conversion.seed = options.dither_seed;
        mp::ProcessedGraph graph{*source,          sink,
                                 negotiated.accepted, period,
                                 conversion,       &hooks,
                                 buffering(options),
                                 chain.empty() ? nullptr : &chain};
        graph.set_position(queue != nullptr ? queue->position() : 0);
        const RunOutcome rc =
            run_graph(graph, hooks, negotiated.accepted, limit, &options, queue,
                      &source_format);
        // What each stage made of it, in the stage's own words. `dsp_gain`
        // reports the loudest sample it saw, which is the cheapest evidence
        // that the chain was in the path at all.
        if (options.verbose) {
            for (std::size_t i = 0; i < chain.size(); ++i) {
                for (const std::string& line : chain.at(i).describe()) {
                    std::printf("%-10s %s\n", chain.at(i).name().c_str(), line.c_str());
                }
            }
        }
        return rc;
    }

    mp::PassthroughGraph graph{*source,   sink,
                               negotiated.accepted, period,
                               negotiated.fidelity, &hooks, buffering(options)};
    graph.set_position(queue != nullptr ? queue->position() : 0);
    return run_graph(graph, hooks, negotiated.accepted, limit, &options, queue,
                     &source_format);
}



/// What `decode` writes to, when it is asked to run Path B.
///
/// There is no device to negotiate with, so nothing gets to refuse: the wire
/// format is what the chain produced, quantised into the source's own container
/// unless `--bits` named another. That default is the point -- it makes a
/// chainless `--path processed` run comparable byte for byte with a bit-exact
/// one, which is how the gain, the dither and the shapers get held to anything.
mp::Format decode_wire_format(const mp::Format& source, const mp::DspChain& chain,
                              const Options& options)
{
    mp::Format wire = chain.empty() ? source : chain.output_format();
    wire.sample_type = source.sample_type;
    wire.valid_bits = source.valid_bits;
    wire.encoding = mp::Encoding::pcm;
    if (options.bits_given || options.float_source) {
        const mp::Format asked = requested_format(options);
        wire.sample_type = asked.sample_type;
        wire.valid_bits = asked.valid_bits;
    }
    return wire;
}

/// Path B without a device: source -> the f64 bus -> the chain -> the wire.
///
/// **This is the arithmetic the whole tree is about, and until now nothing
/// could hash it.** `--path processed` and `--dsp` were accepted here and then
/// ignored: a -6 dB gain and a resample to 192 kHz left the SHA-256 exactly
/// where it started, so every claim about the resampler, the dither and the
/// shapers rested on listening rather than on bytes, and a build that changed
/// one of them by an LSB would have passed every test in the tree.
int decode_processed(mp::ISource& source, mp::DspChain& chain, const Options& options,
                     std::FILE* raw)
{
    const mp::Format source_format = source.format();
    // The block the chain is sized for. A device would say how big a period is;
    // here nothing does, so `--block` says, and what it is set to must not
    // change a single byte of the output: a stage whose answer depends on the
    // size of the piece it was handed is a broken stage, and on a real device
    // that bug would only appear on somebody else's hardware.
    //
    // It comes before the wire format because a chain says nothing about its
    // output until it has been configured, and the wire is read off that.
    const std::uint32_t block = options.block;
    if (block == 0) {
        std::fprintf(stderr, "--block must be at least 1 frame\n");
        return 1;
    }
    std::string why;
    if (!chain.empty() && !chain.configure(mp::dsp_bus_format(source_format), block, why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    const mp::Format wire = decode_wire_format(source_format, chain, options);

    mp::ConvertConfig conversion;
    conversion.gain = options.gain;
    conversion.dither = options.dither;
    conversion.shaping = options.shaping;
    conversion.seed = options.dither_seed;
    mp::Processor proc{source_format, wire, block, conversion,
                       chain.empty() ? nullptr : &chain};
    if (!proc.possible()) {
        std::fprintf(stderr, "nothing here converts %s to %s\n",
                     mp::describe(source_format).c_str(), mp::describe(wire).c_str());
        return 1;
    }

    mp::win::Sha256 hash;
    std::vector<std::uint8_t> in(proc.input_bytes());
    std::vector<std::uint8_t> out(proc.output_bytes());
    std::uint64_t total = 0;
    std::uint64_t read_frames = 0;

    const auto keep = [&](std::uint32_t frames) {
        if (frames == 0) {
            return;
        }
        const std::size_t bytes = static_cast<std::size_t>(frames) * proc.wire_frame_bytes();
        hash.update(out.data(), bytes);
        if (raw != nullptr) {
            std::fwrite(out.data(), 1, bytes, raw);
        }
        total += bytes;
    };

    for (;;) {
        const std::size_t got = source.read(in.data(), in.size());
        const auto frames = static_cast<std::uint32_t>(got / proc.source_frame_bytes());
        if (frames == 0) {
            break;
        }
        read_frames += frames;
        std::uint32_t produced = 0;
        if (!proc.run(in.data(), frames, out.data(), produced)) {
            std::fprintf(stderr, "the chain failed at frame %llu\n",
                         static_cast<unsigned long long>(read_frames));
            return 1;
        }
        keep(produced); // 0 is normal: a stage may still be filling its history
    }

    // A resampler holds a filter's worth of the last audio and it is as much
    // the file as the rest. A decode that stopped at the last input frame would
    // hash a truncated file and call it the answer.
    while (!proc.flush_done()) {
        std::uint32_t produced = 0;
        const bool again = proc.flush(out.data(), produced);
        keep(produced);
        if (!again) {
            break;
        }
    }

    const std::uint32_t stride = proc.wire_frame_bytes();
    std::printf("file       %s\n", options.file.c_str());
    // Said out loud, because a chain or a gain overrides the policy: somebody
    // who typed `--path exact --dsp eq` asked for two things that contradict
    // each other, and a stage exists in order to change the samples.
    std::printf("path       PROCESSED -- the samples are changed  [--path %s]\n",
                mp::path_policy_name(options.path));
    std::printf("source     %s\n", mp::describe(source_format).c_str());
    std::printf("wire       %s%s\n", mp::describe(wire).c_str(),
                proc.lossy() ? "" : "  [nothing thrown away]");
    if (!chain.empty()) {
        std::printf("chain      ");
        for (std::size_t i = 0; i < chain.size(); ++i) {
            std::printf("%s%s", i == 0 ? "" : " -> ", chain.at(i).name().c_str());
        }
        std::printf("\n");
        if (const std::uint32_t delay = chain.latency_frames(); delay != 0) {
            std::printf("latency    %u frames the chain adds\n", delay);
        }
    }
    std::printf("block      %u frames\n", block);
    std::printf("gain       %.6f\n", options.gain);
    std::printf("dither     %s%s\n", mp::dither_kind_name(options.dither),
                proc.lossy() ? "" : "  [not reached -- the conversion is exact]");
    std::printf("frames     %llu in, %llu out (%.3f s)\n",
                static_cast<unsigned long long>(read_frames),
                static_cast<unsigned long long>(total / stride),
                static_cast<double>(total / stride) / wire.sample_rate);
    std::printf("bytes      %llu\n", static_cast<unsigned long long>(total));
    std::printf("sha256     %s\n", hash.hex().c_str());
    if (options.verbose) {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            for (const std::string& line : chain.at(i).describe()) {
                std::printf("%-10s %s\n", chain.at(i).name().c_str(), line.c_str());
            }
        }
    }
    return 0;
}

/// Hashes whatever the resolution rules opened.
///
/// **A source rather than a decoder vtable**, so this measures what actually
/// plays. A container and a codec are two modules now, and a `decode` that could
/// only reach the one-module kind would be a measurement of the half being
/// retired.
int decode(mp::ISource& source, const mp::win::ModuleRegistry& registry,
           const Options& options)
{
    if (options.seek != 0 && !source.seek(options.seek)) {
        std::fprintf(stderr, "cannot seek %s to frame %llu\n", options.file.c_str(),
                     static_cast<unsigned long long>(options.seek));
        return 1;
    }
    if (options.seek != 0) {
        std::printf("seek       to frame %llu\n",
                    static_cast<unsigned long long>(options.seek));
    }

    const mp::Format format = source.format();
    const std::size_t stride = mp::frame_bytes(format);

    mp::DspChain chain;
    std::string why;
    for (const std::string& spec : options.dsp) {
        if (!add_dsp_stage(registry, spec, chain, why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
    }

    // mp::win::open_utf8 rather than fopen: the path arrives as UTF-8, and a
    // narrow CRT call cannot open half the files on this machine. Rather than
    // std::ofstream either, which brings iostreams and the locale facets behind
    // them into a binary whose only use for them is writing bytes.
    std::FILE* raw = nullptr;
    if (!options.raw.empty()) {
        raw = mp::win::open_utf8(options.raw, L"wb");
        if (raw == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", options.raw.c_str());
            return 1;
        }
    }

    // The same question `play` asks, with the file standing in for the device.
    // Nothing here can refuse a format, so the only fidelity on offer is exact
    // and what decides is the policy and whether anything alters the samples.
    if (mp::use_processed(options.path, mp::Fidelity::exact,
                          !chain.empty() || options.gain != 1.0)) {
        const int rc = decode_processed(source, chain, options, raw);
        if (raw != nullptr) {
            std::fclose(raw);
        }
        return rc;
    }

    mp::win::Sha256 hash;
    std::vector<std::uint8_t> chunk(64 * 1024 / stride * stride);
    std::uint64_t total = 0;
    for (;;) {
        const std::size_t got = source.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        hash.update(chunk.data(), got);
        if (raw != nullptr) {
            std::fwrite(chunk.data(), 1, got, raw);
        }
        total += got;
    }
    if (raw != nullptr) {
        std::fclose(raw);
    }

    std::printf("file       %s\n", options.file.c_str());
    std::printf("format     %s\n", mp::describe(format).c_str());
    std::printf("path       bit-exact -- these are the decoder's own bytes\n");
    std::printf("frames     %llu (%.3f s)\n",
                static_cast<unsigned long long>(total / stride),
                static_cast<double>(total / stride) / format.sample_rate);
    if (source.length_frames() != 0 && source.length_frames() != total / stride) {
        std::printf("           header said %llu\n",
                    static_cast<unsigned long long>(source.length_frames()));
    }
    std::printf("bytes      %llu\n", static_cast<unsigned long long>(total));
    std::printf("sha256     %s\n", hash.hex().c_str());
    return 0;
}


// --------------------------------------------------------------------------
// The measuring apparatus, from here to the end of this namespace.
//
// `compare` holds a decode against the audio that was encoded; `verify` plays a
// file into a virtual cable and reads the other end. Both are how the hard bugs
// in this project were found and neither is needed to play a file, so a build
// that ships leaves them out: about 26 KB of the probe, most of it the analysis
// in mediaperch_core:compare.obj and mediaperch_win:verify.obj, which are in
// static libraries and therefore never linked once nothing calls them.
//
// Compiled in for Debug always, and for any configuration when the build is
// configured with -D MEDIAPERCH_DIAGNOSTICS=ON. The libraries themselves are
// built and unit-tested either way -- it is only the shipping executable that
// does without.
#if MEDIAPERCH_DIAGNOSTICS


/// Reads a whole file as interleaved float, whatever the decoder hands back.
///
/// The conversion is the encoder's: an integer sample divided by the magnitude
/// of its own full scale, which is what every encoder does to its input and
/// therefore the only division that makes a comparison with the source mean
/// anything. It is a conversion, and it belongs here in a measuring tool rather
/// than in any decoder.
bool read_as_float(mp::ISource& input, mp::Format& format, std::vector<float>& out)
{
    format = input.format();
    const std::size_t stride = mp::frame_bytes(format);
    if (stride == 0) {
        return false;
    }
    std::vector<std::uint8_t> chunk(64 * 1024 / stride * stride);
    std::vector<std::uint8_t> raw;
    for (;;) {
        const std::size_t got = input.read(chunk.data(), chunk.size());
        if (got == 0) {
            break;
        }
        raw.insert(raw.end(), chunk.begin(), chunk.begin() + got);
    }

    const std::size_t samples = raw.size() / mp::container_bytes(format.sample_type);
    out.resize(samples);
    const std::uint8_t* p = raw.data();
    for (std::size_t i = 0; i < samples; ++i) {
        switch (format.sample_type) {
        case mp::SampleType::u8:
            out[i] = (static_cast<float>(p[i]) - 128.0F) / 128.0F;
            break;
        case mp::SampleType::s16: {
            std::int16_t v = 0;
            std::memcpy(&v, p + i * 2, 2);
            out[i] = static_cast<float>(v) / 32768.0F;
            break;
        }
        case mp::SampleType::s24_packed: {
            const std::int32_t v = static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(p[i * 3 + 2]) << 24) |
                (static_cast<std::uint32_t>(p[i * 3 + 1]) << 16) |
                (static_cast<std::uint32_t>(p[i * 3 + 0]) << 8));
            out[i] = static_cast<float>(v >> 8) / 8388608.0F;
            break;
        }
        case mp::SampleType::s24_in_32:
        case mp::SampleType::s32: {
            std::int32_t v = 0;
            std::memcpy(&v, p + i * 4, 4);
            out[i] = static_cast<float>(static_cast<double>(v) / 2147483648.0);
            break;
        }
        case mp::SampleType::f32:
            std::memcpy(&out[i], p + i * 4, 4);
            break;
        case mp::SampleType::f64: {
            double v = 0.0;
            std::memcpy(&v, p + i * 8, 8);
            out[i] = static_cast<float>(v);
            break;
        }
        case mp::SampleType::none:
            return false;
        }
    }
    return true;
}

void report(const char* who, const mp::Comparison& m)
{
    std::printf("%-8s frames %llu  %.2f dB  rms %.3e  peak %.4f  lag %+d (%.1f dB clear)\n", who,
                static_cast<unsigned long long>(m.frames_subject), m.snr_db, m.rms_error,
                m.peak_subject, m.lag, m.lag_margin_db);
    if (m.bands_checked != 0) {
        std::printf("         %u bands checked, worst %.2f dB at %u Hz\n", m.bands_checked,
                    m.worst_band_db, m.worst_band_hz);
    }
    std::printf("         channels ");
    for (unsigned c = 0; c < m.best_for.size(); ++c) {
        std::printf("%u<-%u ", c, m.best_for[c]);
    }
    std::printf(" (%.1f dB clear)\n", m.channel_margin_db);
}

int compare_command(mp::win::EngineHost& host, const Options& options)
{
    // The source goes through the resolution rules like anything else, so this
    // works for a WAV, a FLAC or anything else lossless -- and the module that
    // reads it is measured elsewhere, which is what makes it usable as truth.
    auto source_input = open_file(host, options.source, {}, "read the source");
    if (!source_input) {
        return 1;
    }
    mp::Format source_format;
    std::vector<float> source;
    if (!read_as_float(*source_input, source_format, source)) {
        std::fprintf(stderr, "cannot read the source %s\n", options.source.c_str());
        return 1;
    }

    auto subject_input = open_file(host, options.file, options.decoder_id, "decode");
    if (!subject_input) {
        return 1;
    }
    mp::Format subject_format;
    std::vector<float> subject;
    if (!read_as_float(*subject_input, subject_format, subject)) {
        std::fprintf(stderr, "cannot decode %s\n", options.file.c_str());
        return 1;
    }

    std::printf("source     %s\n", mp::describe(source_format).c_str());
    std::printf("subject    %s\n", mp::describe(subject_format).c_str());

    if (subject_format.channels != source_format.channels ||
        subject_format.sample_rate != source_format.sample_rate) {
        std::fprintf(stderr,
                     "FAIL  the decode is %u ch at %u Hz and the source is %u ch at %u Hz\n",
                     subject_format.channels, subject_format.sample_rate,
                     source_format.channels, source_format.sample_rate);
        return 1;
    }

    const unsigned channels = source_format.channels;
    const std::uint64_t source_frames = source.size() / channels;
    const std::uint64_t subject_frames = subject.size() / channels;

    const mp::Comparison mine =
        mp::compare(source.data(), source_frames, subject.data(), subject_frames, channels,
                    source_format.sample_rate, options.band_limit_hz, options.lag_window);
    report("ours", mine);

    // A second decoder, measured the same way. It cannot rank the two -- the
    // encoder's loss is common to both and is millions of times larger than the
    // difference between them -- but it does say whether this decoder gave
    // anything away that the other one kept.
    bool rival_ok = true;
    if (options.rival_id != "none") {
        std::string rival_id;
        std::string rival_why;
        auto rival_input =
            host.open_source(options.file, options.rival_id, rival_id, rival_why);
        if (rival_input) {
            mp::Format rival_format;
            std::vector<float> other;
            if (read_as_float(*rival_input, rival_format, other) &&
                rival_format.channels == channels) {
                const mp::Comparison theirs = mp::compare(
                    source.data(), source_frames, other.data(), other.size() / channels, channels,
                    source_format.sample_rate, options.band_limit_hz, options.lag_window);
                report(rival_label(rival_id), theirs);
                // Equal to within a part in ten thousand, which for these two is
                // eleven orders of magnitude of headroom: the point is to catch a
                // decoder that lost something, not to declare a winner.
                if (mine.rms_error > theirs.rms_error * 1.0001) {
                    std::printf("FAIL  further from the source than %s: %.6e against %.6e\n",
                                rival_id.c_str(), mine.rms_error, theirs.rms_error);
                    rival_ok = false;
                }
                // And the two held against each other rather than against the
                // source, which is a different question with a different
                // answer. Substituted noise is the case that needs it: a
                // noise-substituted band is arbitrary by design, so two
                // decoders can put different noise in one and sit the same
                // distance from the source while disagreeing completely with
                // each other. The source cannot see that. This can.
                const mp::Comparison between =
                    mp::compare(other.data(), other.size() / channels, subject.data(),
                                subject_frames, channels, source_format.sample_rate, 0,
                                options.lag_window);
                std::printf("against  %s directly: %.2f dB\n", rival_label(rival_id),
                            between.snr_db);
                if (between.snr_db < options.vs_rival_db) {
                    std::printf("FAIL  %.2f dB from %s, and %.2f was required\n", between.snr_db,
                                rival_id.c_str(), options.vs_rival_db);
                    rival_ok = false;
                }
            }
        }
    }

    mp::Requirements required;
    required.exact_length = options.exact_length;
    required.lag_zero = options.exact_length;
    required.min_snr_db = options.min_snr_db;
    required.min_lag_margin_db = options.min_lag_margin_db;
    required.min_channel_margin_db = options.min_channel_margin_db;
    required.max_peak_ratio = options.max_peak_ratio;
    required.max_band_db = options.band_tol_db;

    const auto problems = mp::failures(mine, required);
    for (const auto& why : problems) {
        std::printf("FAIL  %s\n", why.c_str());
    }
    if (!problems.empty() || !rival_ok) {
        return 1;
    }
    std::printf("PASS\n");
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

int verify(const MpSinkVtbl& sink_vtbl, mp::ISource& input, const Options& options)
{

    // ---- decode ----------------------------------------------------------
    const mp::Format source_format = input.format();
    const std::size_t source_stride = mp::frame_bytes(source_format);
    const std::uint64_t cap_frames =
        static_cast<std::uint64_t>(options.seconds) * source_format.sample_rate;

    std::vector<std::uint8_t> source_bytes;
    source_bytes.reserve(static_cast<std::size_t>(cap_frames) * source_stride);
    {
        std::vector<std::uint8_t> chunk(64 * 1024 / source_stride * source_stride);
        while (source_bytes.size() < cap_frames * source_stride) {
            const std::size_t got = input.read(chunk.data(), chunk.size());
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
    if (!device_id_for(sink_vtbl, options, device_id)) {
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

    const auto negotiated = mp::negotiate_best(sink, source_format, options.path);
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
                if (std::FILE* out = mp::win::open_utf8(where, L"wb")) {
                    std::fwrite(data, 1, bytes, out);
                    std::fclose(out);
                }
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


#else // MEDIAPERCH_DIAGNOSTICS

/// What the measuring commands do in a build that left them out.
///
/// Not silence, and not a pretend success: a test harness has to be able to
/// tell "this build cannot answer that" from "the answer was wrong", so this
/// says which it is and exits 77 -- the code a test runner reads as skipped.
int no_diagnostics(const char* command)
{
    std::fprintf(stderr,
                 "`%s` is a measuring command, and this build does not have one.\n"
                 "Configure with -D MEDIAPERCH_DIAGNOSTICS=ON to put the measuring\n"
                 "apparatus back. A Debug build always has it.\n",
                 command);
    return 77;
}

#endif // MEDIAPERCH_DIAGNOSTICS

} // namespace

int main(int argc, char** argv)
{
    const mp::win::ConsoleUtf8 console;

    // Windows hands `main` its arguments in the process code page. Everything
    // past this point -- the ABI, the module boundary, every path in this file
    // -- is UTF-8, so take the arguments from the command line Windows actually
    // kept rather than from the lossy copy the CRT made.
    const std::vector<std::string> args = mp::win::command_line_utf8();
    std::vector<char*> utf8_argv;
    if (static_cast<int>(args.size()) == argc) {
        utf8_argv.reserve(args.size());
        for (const std::string& arg : args) {
            utf8_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv = utf8_argv.data();
    }

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

    mp::win::ModuleRegistry registry;
    registry.scan(mp::win::module_directory());

    if (options.command == "modules") {
        for (const MpModuleDesc* desc : registry.all()) {
            std::printf("%-16s %-38s %-7s priority %3u  v%u.%u.%u%s\n", desc->id,
                        desc->name, mp::module_kind_name(desc->kind), desc->priority,
                        desc->version >> 22,
                        (desc->version >> 12) & 0x3FF, desc->version & 0xFFF,
                        (desc->flags & MP_MODULE_NO_UNLOAD) != 0 ? "  [no-unload]" : "");
        }
        return 0;
    }

    for (const std::string& spec : options.dsp) {
        if (spec == "list") {
            return list_dsp_modules(registry);
        }
    }

    const MpSinkVtbl* sink_vtbl = registry.sink(options.sink_id);
    if (sink_vtbl == nullptr) {
        std::fprintf(stderr, "no sink module%s%s beside the executable\n",
                     options.sink_id.empty() ? "" : " called ",
                     options.sink_id.c_str());
        return 1;
    }
    const MpSinkVtbl& sink = *sink_vtbl;

    if (options.command == "claims") {
        if (options.file.empty()) {
            std::fprintf(stderr, "claims needs --file\n");
            return 1;
        }
        // **Resolution has two questions and this prints both answers.** A
        // container is claimed on its first bytes; a codec is looked up on what
        // the container then says is inside. The first without the second would
        // show the question being asked and not the answer.
        //
        // Every claimant's opinion, not just the winner's: "several read it"
        // and "this is the one you get" are different facts, and a table of
        // coverage that cannot tell them apart is a table that overstates.
        if (!mp::win::ModuleRegistry::readable(options.file)) {
            std::fprintf(stderr, "no such file, or it is empty: %s\n",
                         options.file.c_str());
            return 1;
        }
        const auto demuxers = registry.demuxers_for(options.file, {});
        if (demuxers.empty()) {
            std::printf("nothing here reads this container\n");
        }
        for (const auto& candidate : demuxers) {
            std::printf("%-16s %3u  %s\n", candidate.desc->id, candidate.score,
                        candidate.desc->name);
            // Claiming a container and reading it are different, so this opens
            // it. That is allowed here for the reason it is allowed in `open`
            // and not in `probe`: there is no four-kilobyte window once the
            // file is ours to read.
            mp::Demux demux;
            const MpResult opened = demux.open(*candidate.vtbl, options.file.c_str());
            if (opened != MP_OK) {
                std::printf("  opens: %s\n", mp::result_name(opened));
                continue;
            }
            for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
                MpStreamInfo info{};
                if (!demux.stream_info(i, info)) {
                    continue;
                }
                std::vector<std::uint8_t> config;
                (void)demux.stream_config(i, config);
                const char* takes = "nothing here decodes it";
                if ((info.flags & MP_STREAM_SELF_DECODES) != 0u) {
                    takes = "this module, itself";
                } else if (const MpCodecVtbl* chosen = registry.codec_for(
                               info.codec, config.empty() ? nullptr : config.data(),
                               static_cast<std::uint32_t>(config.size()))) {
                    // The vtable does not carry the module's name, so the
                    // module is found again by identity among the loaded ones.
                    for (const MpModuleDesc* desc : registry.all()) {
                        if (desc->kind == MP_KIND_CODEC && desc->vtbl == chosen) {
                            takes = desc->id;
                            break;
                        }
                    }
                }
                std::printf("  stream %u  %-8s %-8s -> %s\n", i,
                            mp::stream_kind_name(info.kind), mp::codec_name(info.codec),
                            takes);
            }
        }

        return 0;
    }

    if (options.command == "devices") {
        return list_devices(sink);
    }
    if (options.command == "negotiate") {
        return negotiate(sink, options);
    }
    if (options.command == "show") {
        return show(sink, registry, options);
    }
    if (options.command == "play") {
        return play(sink, registry, options);
    }
    if (options.command == "decode" || options.command == "verify" ||
        options.command == "compare" || options.command == "loudness") {
        if (options.file.empty()) {
            std::fprintf(stderr, "%s needs --file\n", options.command.c_str());
            return 1;
        }
        // **All four resolve the way the engine does.** Two ways of opening a
        // file that were not the same way would be worse than either -- these
        // are the commands whose whole job is to say what came out, so they
        // have to be looking at what plays.
        mp::LogRing log;
        (void)log.listen([&options](const std::string& line) {
            if (options.verbose) {
                std::fprintf(stderr, "%s\n", line.c_str());
            }
        });
        mp::win::EngineHost host{registry, log};

#if MEDIAPERCH_DIAGNOSTICS
        // `compare` opens two files and names them itself, so it is the one
        // command that does its own resolving.
        if (options.command == "compare") {
            if (options.source.empty()) {
                std::fprintf(stderr, "compare needs --source\n");
                return 1;
            }
            return compare_command(host, options);
        }
#endif

        std::string opened_by;
        std::string why;
        auto source = host.open_source(options.file, options.decoder_id, opened_by, why);
        if (!source) {
            std::fprintf(stderr, "cannot %s %s: %s\n", options.command.c_str(),
                         options.file.c_str(),
                         why.empty() ? "nothing here reads it" : why.c_str());
            return 1;
        }
        std::printf("decoder    %s%s\n", opened_by.c_str(),
                    options.decoder_id.empty() ? "" : "  [forced]");

        if (options.command == "decode") {
            return decode(*source, registry, options);
        }
#if MEDIAPERCH_HAS_LOUDNESS
        if (options.command == "loudness") {
            return loudness(*source, options);
        }
#else
        if (options.command == "loudness") {
            std::fprintf(stderr, "this build has no loudness meter\n");
            return 1;
        }
#endif
#if MEDIAPERCH_DIAGNOSTICS
        return verify(sink, *source, options);
#else
        return no_diagnostics(options.command.c_str());
#endif
    }

    usage();
    return 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/player.hpp"

#include "mediaperch/wiring.hpp"

#include "mediaperch/result.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <utility>

namespace mp {
namespace {

/// How often the engine thread looks up from the graph. Short enough that
/// "stop" is not noticeably late, long enough that this is not a spin.
constexpr auto k_poll = std::chrono::milliseconds{10};

std::string trimmed(std::string_view s)
{
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

std::vector<std::string> split(const std::string& s, char by)
{
    std::vector<std::string> out;
    for (std::size_t at = 0; at <= s.size();) {
        const std::size_t next = std::min(s.find(by, at), s.size());
        const std::string piece = trimmed(std::string_view{s}.substr(at, next - at));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        at = next + 1;
    }
    return out;
}

std::string joined(const std::vector<std::string>& items, char by)
{
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out.push_back(by);
        }
        out += item;
    }
    return out;
}

/// A number, and no opinion about whether it is a sensible one.
///
/// **The range checks that used to be here are gone on purpose.** This tree's
/// rule is C++'s own, restated for the person at the other end: give the user
/// the choice even when the user may be wrong. A setting that refuses an
/// unusual value refuses somebody's reason for wanting it, and the reason is
/// not knowable from here -- a gain above unity is clipping to one listener and
/// recovered headroom to another; one ring period is the lowest latency a
/// machine can do and a stutter on the next machine along.
///
/// What is still refused is text that is not a number, and text whose number
/// the destination cannot hold. Neither is a judgement about the value: the
/// first has no value in it, and the second would arrive as a different number
/// than the one that was typed.
bool as_number(const std::string& text, double& out)
{
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

/// The same, for a field that is unsigned. `limit` is the destination type's own
/// maximum and is never a policy -- see `as_number`.
bool as_unsigned(const std::string& text, double limit, double& out)
{
    return as_number(text, out) && out >= 0.0 && out <= limit;
}

bool as_bool(const std::string& text, bool& out)
{
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

// --------------------------------------------------------------------------
// The playlist
// --------------------------------------------------------------------------

/// The files the engine was given, opened one at a time.
///
/// Lazy for the same reason the probe's is: a playlist of five hundred entries
/// is five hundred open decoders if the queue is handed a list, and none of
/// them are needed until the one before finishes.
class Player::Playlist final : public IPlaylist {
public:
    Playlist(IEngineHost& host, std::vector<std::string> files)
        : host_(&host), files_(std::move(files))
    {
        opened_.resize(files_.size());
    }

    ISource* at(std::size_t index) override
    {
        IMedia* media = at_media(index);
        return media != nullptr ? &media->audio() : nullptr;
    }

    /// The whole file, for the caller that wants the picture too.
    ///
    /// `at` is `IPlaylist`'s and hands back the audio because that is what a
    /// `Queue` reads; this is the same object with the rest of it still
    /// attached, and both point into one demuxer with one position (§4).
    IMedia* at_media(std::size_t index)
    {
        if (index >= files_.size()) {
            return nullptr;
        }
        if (opened_[index]) {
            return opened_[index].get();
        }
        std::string why;
        auto media = host_->open_media(files_[index], why);
        if (!media) {
            // Recorded rather than fatal: a playlist that silently plays four
            // of its five entries is worse than one that says which it skipped.
            host_->log("skipping " + files_[index] + ": " + why);
            return nullptr;
        }
        opened_[index] = std::move(media);
        return opened_[index].get();
    }

    [[nodiscard]] const std::string& decoder_name(std::size_t index) const
    {
        static const std::string none;
        return index < opened_.size() && opened_[index] ? opened_[index]->decoder() : none;
    }
    [[nodiscard]] const std::string& path(std::size_t index) const
    {
        static const std::string none;
        return index < files_.size() ? files_[index] : none;
    }

private:
    IEngineHost* host_;
    std::vector<std::string> files_;
    std::vector<std::unique_ptr<IMedia>> opened_;
};

// --------------------------------------------------------------------------

Player::Player(IEngineHost& host) : host_(&host) {}

Player::~Player()
{
    shutdown();
}

void Player::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    thread_ = std::thread{[this] { run(); }};
}

void Player::shutdown()
{
    if (!started_) {
        return;
    }
    quit_.store(true, std::memory_order_release);
    stop_wanted_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    started_ = false;
}

// --------------------------------------------------------------------------
// What a shell asks for
// --------------------------------------------------------------------------

void Player::play(std::vector<std::string> files, std::size_t first)
{
    {
        const std::lock_guard lock{mutex_};
        files_ = files;
        requests_.clear();
        requests_.push_back(Request{std::move(files), first, 0});
    }
    // The engine thread is inside a graph, and this is how it is told to leave.
    stop_wanted_.store(true, std::memory_order_release);
    wake_.notify_all();
}

void Player::enqueue(const std::vector<std::string>& files)
{
    const std::lock_guard lock{mutex_};
    files_.insert(files_.end(), files.begin(), files.end());
    // Not a request: what is playing keeps playing. The queue reads the
    // playlist through `Playlist::at`, which is built from `files_` when a run
    // starts, so an addition reaches the *next* run rather than this one.
}

void Player::clear()
{
    {
        const std::lock_guard lock{mutex_};
        files_.clear();
        requests_.clear();
    }
    stop_wanted_.store(true, std::memory_order_release);
    wake_.notify_all();
}

void Player::pause()
{
    const std::lock_guard lock{mutex_};
    if (graph_a_ != nullptr) {
        graph_a_->pause();
    } else if (graph_b_ != nullptr) {
        graph_b_->pause();
    } else {
        return;
    }
    state_ = ipc::State::paused;
}

void Player::resume()
{
    const std::lock_guard lock{mutex_};
    if (graph_a_ != nullptr) {
        graph_a_->resume();
    } else if (graph_b_ != nullptr) {
        graph_b_->resume();
    } else {
        return;
    }
    state_ = ipc::State::playing;
}

void Player::stop()
{
    {
        const std::lock_guard lock{mutex_};
        requests_.clear();
    }
    stop_wanted_.store(true, std::memory_order_release);
    wake_.notify_all();
}

bool Player::seek(std::int64_t frames, bool relative)
{
    // Held for the whole call: the graph may not be destroyed underneath a
    // seek, and the engine thread clears these pointers under the same lock
    // before it destroys anything.
    const std::lock_guard lock{mutex_};
    const std::uint64_t now = graph_a_ != nullptr   ? graph_a_->position_frames()
                              : graph_b_ != nullptr ? graph_b_->position_frames()
                                                    : 0;
    if (graph_a_ == nullptr && graph_b_ == nullptr) {
        return false;
    }
    const std::int64_t target = relative ? static_cast<std::int64_t>(now) + frames : frames;
    const auto clamped = static_cast<std::uint64_t>(std::max<std::int64_t>(0, target));
    return graph_a_ != nullptr ? graph_a_->seek(clamped) : graph_b_->seek(clamped);
}

void Player::next()
{
    const std::lock_guard lock{mutex_};
    if (queue_ != nullptr) {
        // The queue is asked, not the graph. The device never notices.
        queue_->skip();
    }
}

void Player::previous()
{
    const std::lock_guard lock{mutex_};
    if (queue_ == nullptr) {
        return;
    }
    // What a "previous" button means everywhere: the start of this track,
    // unless you have only just got here, in which case the one before.
    const std::uint64_t start = queue_->item_start();
    const std::uint64_t here = graph_a_ != nullptr   ? graph_a_->position_frames()
                               : graph_b_ != nullptr ? graph_b_->position_frames()
                                                     : start;
    const std::uint64_t grace = queue_->format().sample_rate * 3ull;
    const std::uint64_t target =
        (here > start + grace || !queue_->has_previous()) ? start : queue_->previous_start();
    if (graph_a_ != nullptr) {
        (void)graph_a_->seek(target);
    } else if (graph_b_ != nullptr) {
        (void)graph_b_->seek(target);
    }
}

ipc::Status Player::status() const
{
    const std::lock_guard lock{mutex_};
    ipc::Status s;
    s.state = state_;
    s.index = index_;
    s.count = static_cast<std::uint32_t>(files_.size());
    s.track = track_;
    s.decoder = decoder_;
    s.device = device_;
    s.source = source_;
    s.wire = wire_;
    s.fidelity = fidelity_;
    s.processed = processed_;
    s.error = error_;
    s.position = position_;
    if (queue_ != nullptr) {
        s.length = queue_->length_frames();
        s.index = static_cast<std::uint32_t>(queue_->index());
        // From the playlist rather than from what the run started with: a queue
        // crosses track boundaries without telling the graph, which is the
        // point of it, so the name has to be looked up and not remembered.
        if (s.index < files_.size()) {
            s.track = files_[s.index];
        }
    }
    s.frames_rendered = total_frames_;
    s.underruns = total_underruns_;
    if (graph_a_ != nullptr) {
        s.position = graph_a_->position_frames();
        s.frames_rendered += graph_a_->stats().frames_rendered;
        s.underruns += graph_a_->stats().underruns;
    } else if (graph_b_ != nullptr) {
        s.position = graph_b_->position_frames();
        s.frames_rendered += graph_b_->stats().frames_rendered;
        s.underruns += graph_b_->stats().underruns;
    }
    return s;
}

std::vector<std::string> Player::playlist() const
{
    const std::lock_guard lock{mutex_};
    return files_;
}

// --------------------------------------------------------------------------
// Settings
// --------------------------------------------------------------------------

std::vector<ipc::Setting> Player::settings() const
{
    const std::lock_guard lock{mutex_};
    std::vector<ipc::Setting> out;
    const auto row = [&out](const char* key, std::string value, std::string what) {
        out.push_back(ipc::Setting{key, std::move(value), std::move(what)});
    };
    row("device", config_.device,
        "part of an endpoint's name, or empty for the default one");
    row("share", config_.shared ? "shared" : "exclusive",
        "exclusive takes the device and nothing else can make a sound on it");
    row("path", path_policy_name(config_.path),
        "bitexact, exactonly, auto or processed -- what may happen to the samples");
    row("dsp", joined(config_.dsp, ','),
        "stages in the order they run, `name` or `name:key=value,key=value`");
    row("gain", std::to_string(config_.conversion.gain),
        "linear, not decibels. Only on the processed path");
    row("dither", dither_kind_name(config_.conversion.dither),
        "none, rectangular, triangular or gaussian, when a container shrinks");
    // The value is what was typed, because this list is also what gets written
    // to the settings file and a file this program cannot read back is not a
    // settings file. What it resolved to goes in the description, where it is
    // just as visible and cannot be mistaken for an input.
    row("shaping", config_.shaping_spec,
        "0-9 for a binomial order, `shibata[:N]`, or a named curve -- currently " +
            noise_shaping_describe(config_.conversion.shaping,
                                   wire_.sample_rate != 0 ? wire_.sample_rate : 44100));
    row("dither_seed", std::to_string(config_.conversion.seed),
        "so two runs of one file produce the same bytes");
    row("ring_periods", std::to_string(config_.buffering.ring_periods),
        "ring capacity in device periods. Generous by default, because the "
        "worst stall in a file is not knowable before opening it");
    row("prefill_periods", std::to_string(config_.buffering.prefill_periods),
        "how much of the ring is filled before the device starts and before a "
        "seek resumes. Not the whole ring");
    row("wait_timeout", std::to_string(config_.buffering.wait_timeout_ms),
        "how long the render thread waits for a device before calling it gone");
    row("recover", config_.recover ? "1" : "0",
        "rebuild onto an endpoint that comes back, instead of ending the run");
    row("recover_timeout", std::to_string(config_.recover_timeout),
        "seconds to wait for one");
    return out;
}

bool Player::set(const std::string& key, const std::string& value, std::string& why)
{
    bool rebuild = false;
    {
        const std::lock_guard lock{mutex_};
        double number = 0.0;
        if (key == "device") {
            config_.device = value;
            rebuild = true;
        } else if (key == "share") {
            if (value == "shared") {
                config_.shared = true;
            } else if (value == "exclusive") {
                config_.shared = false;
            } else if (!as_bool(value, config_.shared)) {
                why = "share is `exclusive` or `shared`";
                return false;
            }
            rebuild = true;
        } else if (key == "path") {
            if (!path_policy_from_name(value, config_.path)) {
                why = "path is bitexact, exactonly, auto or processed";
                return false;
            }
            rebuild = true;
        } else if (key == "dsp") {
            config_.dsp = split(value, ',');
            rebuild = true;
        } else if (key == "gain") {
            // Linear and unbounded. Above unity clips, below zero inverts,
            // and both are things somebody may want on purpose.
            if (!as_number(value, number)) {
                why = "gain is a number, linear rather than in decibels";
                return false;
            }
            config_.conversion.gain = number;
            rebuild = true;
        } else if (key == "dither") {
            if (!dither_kind_from_name(value, config_.conversion.dither)) {
                why = "dither is none, rectangular, triangular or gaussian";
                return false;
            }
            rebuild = true;
        } else if (key == "shaping") {
            if (!noise_shaping_from_name(value, config_.conversion.shaping)) {
                why = "shaping is 0-9, `shibata[:N]`, or a named curve";
                return false;
            }
            config_.shaping_spec = value;
            rebuild = true;
        } else if (key == "dither_seed") {
            if (!as_unsigned(value, 4294967295.0, number)) {
                why = "dither_seed is a number that fits in 32 bits";
                return false;
            }
            config_.conversion.seed = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "ring_periods") {
            // No floor: the ring sizers already take the larger of what
            // this asks for and what a ring has to be to be one, so zero is
            // "as small as it can be" rather than a broken buffer. No ceiling
            // either -- one that the machine cannot allocate is reported by
            // the run that tried, not refused by a number chosen here.
            if (!as_unsigned(value, 4294967295.0, number)) {
                why = "ring_periods is a whole number of device periods";
                return false;
            }
            config_.buffering.ring_periods = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "prefill_periods") {
            // Zero is a real answer and not a broken one: start the device on
            // whatever the first acquire can be given and let the decode thread
            // catch up, which is a bet on the machine that is the user's to
            // make. Larger than the ring is also real -- the fill stops when
            // the ring is full, so it means "all of it", which is what this
            // program did until it was measured.
            if (!as_unsigned(value, 4294967295.0, number)) {
                why = "prefill_periods is a whole number of device periods";
                return false;
            }
            config_.buffering.prefill_periods = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "wait_timeout") {
            // Zero is a real answer: do not wait at all for a device that
            // has stopped signalling.
            if (!as_unsigned(value, 4294967295.0, number)) {
                why = "wait_timeout is a whole number of milliseconds";
                return false;
            }
            config_.buffering.wait_timeout_ms = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "recover") {
            if (!as_bool(value, config_.recover)) {
                why = "recover is on or off";
                return false;
            }
        } else if (key == "recover_timeout") {
            if (!as_unsigned(value, 4294967295.0, number)) {
                why = "recover_timeout is a whole number of seconds";
                return false;
            }
            config_.recover_timeout = static_cast<unsigned>(number);
        } else {
            why = "there is no setting called `" + key + "`";
            return false;
        }
    }

    // A setting the graph was built from means building it again -- where it
    // stands, from the frame the device stopped on. The same machinery a lost
    // device uses, pointed at a different cause.
    if (rebuild) {
        rebuild_wanted_.store(true, std::memory_order_release);
    }
    return true;
}

// --------------------------------------------------------------------------
// The engine thread
// --------------------------------------------------------------------------

void Player::set_state(ipc::State state)
{
    const std::lock_guard lock{mutex_};
    state_ = state;
}

void Player::note(const std::string& line)
{
    host_->log(line);
}

void Player::run()
{
    while (!quit_.load(std::memory_order_acquire)) {
        Request request;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] {
                return quit_.load(std::memory_order_acquire) || !requests_.empty();
            });
            if (quit_.load(std::memory_order_acquire)) {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        // Consumed here rather than in `play`: whatever asked for this run also
        // asked the previous one to end, and that flag has done its work.
        stop_wanted_.store(false, std::memory_order_release);
        play_request(request);
    }
}

void Player::play_request(const Request& request)
{
    if (request.files.empty()) {
        set_state(ipc::State::stopped);
        return;
    }
    Playlist playlist{*host_, request.files};
    std::size_t first = request.first;
    std::uint64_t from = request.from;

    while (!quit_.load(std::memory_order_acquire)) {
        Queue queue{playlist, first};
        std::string why;
        if (!queue.open(why)) {
            note(why);
            const std::lock_guard lock{mutex_};
            error_ = why;
            state_ = ipc::State::stopped;
            return;
        }
        if (from != 0 && !queue.seek(from)) {
            note("could not resume at frame " + std::to_string(from));
        }
        from = 0;

        // One queue, as many devices and as many graphs as it takes.
        bool rebuilding = false;
        for (;;) {
            std::uint64_t position = 0;
            const RunEnd end = play_run(queue, playlist, position);
            // Read before it is cleared: whether *this* run was the one a
            // setting asked for is what decides whether a failure means the
            // setting was impossible.
            const bool was_rebuilding = rebuilding;
            rebuilding = false;
            if (end == RunEnd::device_lost) {
                if (!config_.recover || !wait_for_device()) {
                    break;
                }
                if (!queue.seek(position)) {
                    note("the device came back but the source cannot seek");
                    break;
                }
                note("resuming at frame " + std::to_string(position));
                continue;
            }
            if (end == RunEnd::rebuild) {
                if (!queue.seek(position)) {
                    note("this source cannot seek, so the new setting waits for the "
                         "next track");
                    break;
                }
                rebuilding = true;
                continue;
            }
            if (end == RunEnd::failed && was_rebuilding) {
                // The new setting cannot be played -- most often bit-exact on a
                // device that will not take the file's own format. Putting it
                // back is better than stopping: somebody who asked for
                // something impossible should hear the music carry on and be
                // told no, not be left in silence.
                {
                    const std::lock_guard lock{mutex_};
                    config_ = applied_;
                }
                note("that setting could not be played, so it has been put back");
                if (queue.seek(position)) {
                    continue;
                }
            }
            if (end == RunEnd::format_change) {
                // The one join a queue will not make, and it is the device's
                // gap rather than the player's: exclusive mode cannot change
                // format without stopping.
                first = queue.index() + 1;
                note("the next track needs the device reopened: " +
                     describe(queue.next_format()));
                break;
            }
            // finished, stopped, or it never started.
            {
                const std::lock_guard lock{mutex_};
                state_ = ipc::State::stopped;
                position_ = position;
            }
            return;
        }
        if (queue.stopped() != QueueStop::format_change) {
            const std::lock_guard lock{mutex_};
            state_ = ipc::State::stopped;
            return;
        }
    }
}

bool Player::build_chain(DspChain& chain, std::string& why)
{
    for (const std::string& spec : config_.dsp) {
        const std::size_t colon = spec.find(':');
        std::string id = spec.substr(0, colon);
        if (id.rfind("dsp_", 0) != 0) {
            id = "dsp_" + id;
        }
        const MpDspVtbl* vtbl = host_->dsp(id);
        if (vtbl == nullptr) {
            why = "no DSP module called " + id + " is loaded";
            return false;
        }
        chain.add(*vtbl, id);
        DspStage& stage = chain.at(chain.size() - 1);
        if (!stage.open()) {
            why = id + " would not open";
            return false;
        }
        if (colon == std::string::npos) {
            continue;
        }
        for (const std::string& setting : split(spec.substr(colon + 1), ',')) {
            const std::size_t equals = setting.find('=');
            if (equals == std::string::npos) {
                why = id + ": `" + setting + "` is not key=value";
                return false;
            }
            if (stage.set(setting.substr(0, equals), setting.substr(equals + 1)) != MP_OK) {
                why = id + " would not take `" + setting + "`";
                return false;
            }
        }
    }
    return true;
}

Player::RunEnd Player::play_run(Queue& queue, Playlist& playlist, std::uint64_t& position)
{
    PlayerConfig config;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
    }
    position = queue.position();
    rebuild_wanted_.store(false, std::memory_order_release);

    const Format source_format = queue.format();
    DspChain chain;
    std::string why;
    if (!build_chain(chain, why)) {
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }

    std::string device;
    Sink sink = host_->open_sink(config.device, config.shared, device, why);
    if (!sink) {
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }

    // **The one route from a source to a device** (wiring.hpp): configure the
    // chain, work out what to offer, negotiate, size the chain to the device's
    // period. `play` and `show` in the probe make the same call. Four copies of
    // this sequence had drifted apart once already, which is how a flag came to
    // be accepted and dropped by one of them.
    Wired wired;
    if (!wire_up(sink, source_format, chain, config.path,
                 config.conversion.gain != 1.0, wired, why)) {
        if (!wired.negotiated.ok) {
            // Phrased here rather than in `wire_up`, because a shell reads one
            // line of status and the probe prints a paragraph to stderr.
            why = "the device would take none of " + std::to_string(wired.negotiated.tried) +
                  " candidate formats for " + describe(wired.offered);
        }
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }
    const Negotiated& negotiated = wired.negotiated;
    const std::uint32_t period = wired.period_frames;

    // A gain changes the samples whatever the formats say, so it counts towards
    // the answer; see use_processed. A chain does too, and is already the
    // reason the policy was forced inside `wire_up`.
    const bool processing = wired.processed;
    {
        const std::lock_guard lock{mutex_};
        // It negotiated, so this configuration is one the device will take.
        applied_ = config;
        error_.clear();
        queue_ = &queue;
        track_ = playlist.path(queue.index());
        decoder_ = playlist.decoder_name(queue.index());
        device_ = device;
        source_ = source_format;
        wire_ = negotiated.accepted;
        fidelity_ = static_cast<std::uint32_t>(negotiated.fidelity);
        processed_ = processing;
        index_ = static_cast<std::uint32_t>(queue.index());
        state_ = ipc::State::playing;
    }
    note("playing " + playlist.path(queue.index()) + " on " + device + " as " +
         describe(negotiated.accepted) + (processing ? " (processed)" : " (bit-exact)"));

    // **The picture, before the graph and after the device.** Before, because
    // a presenter and a decoder take milliseconds to open and the first frame
    // should not wait on them; after, because §9.8.1 hands the decoder the
    // presenter's graphics device and neither exists until now.
    open_video(playlist, queue.index());

    RunEnd end = RunEnd::failed;
    // **An enormous ring is the user's business; a terminated engine is not.**
    // `ring_periods` has no ceiling any more -- `Player::set` says why -- so the
    // buffers below are sized by a number this program did not choose, and a
    // size the machine cannot meet has to come back as a run that failed. The
    // alternative is an exception leaving the engine thread, which ends the
    // process: refusing the setting up front was the old way of avoiding that,
    // and it refused every large value to catch the few impossible ones.
    try {
        if (processing) {
            ProcessedGraph graph{queue,
                                 sink,
                                 negotiated.accepted,
                                 period,
                                 config.conversion,
                                 nullptr,
                                 config.buffering,
                                 chain.empty() ? nullptr : &chain};
            graph.set_position(queue.position());
            {
                const std::lock_guard lock{mutex_};
                graph_b_ = &graph;
            }
            end = pump(graph);
            position = graph.position_frames();
            {
                const std::lock_guard lock{mutex_};
                graph_b_ = nullptr;
                position_ = position;
                total_frames_ += graph.stats().frames_rendered;
                total_underruns_ += graph.stats().underruns;
            }
        } else {
            PassthroughGraph graph{queue,      sink,   negotiated.accepted, period,
                                   negotiated.fidelity, nullptr, config.buffering};
            graph.set_position(queue.position());
            {
                const std::lock_guard lock{mutex_};
                graph_a_ = &graph;
            }
            end = pump(graph);
            position = graph.position_frames();
            {
                const std::lock_guard lock{mutex_};
                graph_a_ = nullptr;
                position_ = position;
                total_frames_ += graph.stats().frames_rendered;
                total_underruns_ += graph.stats().underruns;
            }
        }
    } catch (const std::bad_alloc&) {
        const std::string short_of_memory =
            "not enough memory for the buffers that setting asks for";
        note(short_of_memory);
        const std::lock_guard lock{mutex_};
        // Both, because whichever was set points at a graph that has gone.
        graph_a_ = nullptr;
        graph_b_ = nullptr;
        error_ = short_of_memory;
        end = RunEnd::failed;
    }
    stop_video();
    video_.reset();
    {
        const std::lock_guard lock{mutex_};
        queue_ = nullptr;
    }
    return end;
}


void Player::open_video(Playlist& playlist, std::size_t index)
{
    video_.reset();

    IMedia* media = playlist.at_media(index);
    if (media == nullptr || media->video() == nullptr) {
        return; // most files, and not an error
    }

    // **§9.7.1's headless case**: no window, so the presenter draws into a
    // composition surface a shell composites, and the frame clock is the event
    // that compositor sets. A shell that is not there yet means a picture
    // nobody is looking at, which costs a decode and is what a shell attaching
    // mid-track has to find already running.
    const IMedia::Picture picture = media->picture();
    auto path = std::make_unique<VideoPath>();
    std::string why;
    if (!path->open(*host_, nullptr, *media->video(), picture.info, picture.codec,
                    picture.config, picture.config_bytes, {}, why)) {
        // **Not fatal, and said once.** A file whose picture will not open is a
        // file that plays, which is exactly what it did before there was a
        // video path at all; refusing the track would be a player that got
        // worse when it gained a feature.
        note("playing without the picture: " + why);
        return;
    }
    video_ = std::move(path);
}

void Player::start_video(IAudioClockSource& audio)
{
    if (!video_ || !video_->opened()) {
        return;
    }
    // **After the audio graph has started**, because §8 makes the audio device
    // the master clock and a picture paced against a clock that is not running
    // is a picture paced against a guess.
    std::string why;
    if (!video_->start(audio, why)) {
        note("the picture will not be shown: " + why);
        video_.reset();
    }
}

void Player::stop_video() noexcept
{
    if (video_) {
        video_->stop();
    }
}

template <typename Graph>
Player::RunEnd Player::pump(Graph& graph)
{
    const MpResult started = graph.start();
    if (started != MP_OK) {
        const std::string why = std::string{"could not start: "} + result_name(started);
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        // A device can go between being negotiated with and being started, and
        // that is the same event as losing it later.
        return started == MP_ERR_DEVICE_LOST ? RunEnd::device_lost : RunEnd::failed;
    }

    RunEnd end = RunEnd::finished;
    {
        // The graph, as §8's master clock. Scoped so that the picture is
        // stopped and its thread joined before this goes -- the display loop
        // reads this every turn.
        GraphClock<Graph> audible{graph};
        start_video(audible);

        // **The track the picture belongs to.** A queue plays a playlist
        // gaplessly inside one run, so the audio may move to the next file
        // while the video graph is still reading the last one's feed. Until a
        // boundary rebuilds the picture -- its own step -- the picture stops
        // when the audio leaves the file it came from, which is honest and is
        // not a frame of the wrong film.
        const std::size_t showing = queue_ != nullptr ? queue_->index() : 0;

        while (graph.running()) {
            if (quit_.load(std::memory_order_acquire) ||
                stop_wanted_.load(std::memory_order_acquire)) {
                end = RunEnd::stopped;
                break;
            }
            if (rebuild_wanted_.exchange(false, std::memory_order_acq_rel)) {
                end = RunEnd::rebuild;
                break;
            }
            if (video_ && queue_ != nullptr && queue_->index() != showing) {
                stop_video();
                video_.reset();
            }
            std::this_thread::sleep_for(k_poll);
        }
        stop_video();
    }
    graph.stop();

    if (end == RunEnd::finished) {
        const MpResult error = graph.error();
        if (error == MP_ERR_DEVICE_LOST) {
            note("the device went away");
            const std::lock_guard lock{mutex_};
            error_ = "the device went away";
            return RunEnd::device_lost;
        }
        if (error != MP_OK) {
            const std::string why = std::string{"stopped: "} + result_name(error);
            note(why);
            const std::lock_guard lock{mutex_};
            error_ = why;
            return RunEnd::failed;
        }
    }
    return end;
}

bool Player::wait_for_device()
{
    PlayerConfig config;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
    }
    note("the device was lost; waiting up to " + std::to_string(config.recover_timeout) +
         " s for one to answer");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{config.recover_timeout};
    for (;;) {
        if (quit_.load(std::memory_order_acquire) ||
            stop_wanted_.load(std::memory_order_acquire)) {
            return false;
        }
        if (host_->device_ready(config.device, config.shared)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            note("no endpoint came back");
            const std::lock_guard lock{mutex_};
            error_ = "the device was lost and none came back";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
}

} // namespace mp

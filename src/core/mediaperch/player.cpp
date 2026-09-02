// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/player.hpp"

#include "mediaperch/result.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

bool as_number(const std::string& text, double low, double high, double& out)
{
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || parsed < low || parsed > high) {
        return false;
    }
    out = parsed;
    return true;
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
        names_.resize(files_.size());
    }

    ISource* at(std::size_t index) override
    {
        if (index >= files_.size()) {
            return nullptr;
        }
        if (opened_[index]) {
            return opened_[index].get();
        }
        std::string why;
        std::string decoder;
        auto source = host_->open_source(files_[index], decoder, why);
        if (!source) {
            // Recorded rather than fatal: a playlist that silently plays four
            // of its five entries is worse than one that says which it skipped.
            host_->log("skipping " + files_[index] + ": " + why);
            return nullptr;
        }
        names_[index] = decoder;
        opened_[index] = std::move(source);
        return opened_[index].get();
    }

    [[nodiscard]] const std::string& decoder_name(std::size_t index) const
    {
        static const std::string none;
        return index < names_.size() ? names_[index] : none;
    }
    [[nodiscard]] const std::string& path(std::size_t index) const
    {
        static const std::string none;
        return index < files_.size() ? files_[index] : none;
    }

private:
    IEngineHost* host_;
    std::vector<std::string> files_;
    std::vector<std::unique_ptr<ISource>> opened_;
    std::vector<std::string> names_;
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
    const auto row = [&out](const char* key, std::string value, const char* what) {
        out.push_back(ipc::Setting{key, std::move(value), what});
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
    // What it resolved to rather than what was typed: `shibata` alone and
    // `shibata:5` are different filters and a settings list that showed both as
    // "shibata" would be hiding the difference.
    row("shaping",
        noise_shaping_describe(config_.conversion.shaping,
                               wire_.sample_rate != 0 ? wire_.sample_rate : 44100),
        "0-9 for a binomial order, `shibata[:N]`, or a named curve");
    row("dither_seed", std::to_string(config_.conversion.seed),
        "so two runs of one file produce the same bytes");
    row("ring_periods", std::to_string(config_.buffering.ring_periods),
        "ring capacity in device periods. A busy machine may want more");
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
            if (!as_number(value, 0.0, 8.0, number)) {
                why = "gain is linear, from 0 to 8";
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
                why = "shaping is an order, or a named curve";
                return false;
            }
            rebuild = true;
        } else if (key == "dither_seed") {
            if (!as_number(value, 0.0, 4294967295.0, number)) {
                why = "dither_seed is a number";
                return false;
            }
            config_.conversion.seed = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "ring_periods") {
            if (!as_number(value, 2.0, 4096.0, number)) {
                why = "ring_periods is from 2 to 4096";
                return false;
            }
            config_.buffering.ring_periods = static_cast<std::uint32_t>(number);
            rebuild = true;
        } else if (key == "wait_timeout") {
            if (!as_number(value, 1.0, 600000.0, number)) {
                why = "wait_timeout is in milliseconds";
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
            if (!as_number(value, 0.0, 3600.0, number)) {
                why = "recover_timeout is in seconds";
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

    // What the device is asked for. A chain that resamples or remixes changes
    // what has to reach it, so the rate and the channels come from the chain's
    // output -- but the sample type stays the source's, because the f64 bus is
    // this program's business and offering the device f64 would say the file
    // was something it is not.
    Format offered = source_format;
    PathPolicy policy = config.path;
    if (!chain.empty()) {
        if (!chain.configure(dsp_bus_format(source_format), 4096, why)) {
            note(why);
            const std::lock_guard lock{mutex_};
            error_ = why;
            return RunEnd::failed;
        }
        offered.sample_rate = chain.output_format().sample_rate;
        offered.channels = chain.output_format().channels;
        offered.channel_mask = chain.output_format().channel_mask;
        // A stage exists in order to change the samples. There is nothing left
        // for a policy to decide.
        policy = PathPolicy::processed;
    }

    const auto negotiated = negotiate_best(sink, offered, policy);
    if (!negotiated.ok) {
        why = "the device would take none of " + std::to_string(negotiated.tried) +
              " candidate formats for " + describe(offered);
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }

    std::uint32_t period = 0;
    if (sink.period_frames(period) != MP_OK || period == 0) {
        why = "the device did not report a buffer size";
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }
    if (!chain.empty() && !chain.configure(dsp_bus_format(source_format), period, why)) {
        note(why);
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }

    const bool processing = use_processed(policy, negotiated.fidelity);
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

    RunEnd end = RunEnd::failed;
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
    {
        const std::lock_guard lock{mutex_};
        queue_ = nullptr;
    }
    return end;
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
        std::this_thread::sleep_for(k_poll);
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

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/player.hpp"

#include "mediaperch/profile.hpp"
#include "mediaperch/wiring.hpp"

#include "mediaperch/result.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace mp {
namespace {

/// One `describe` row, turned into a settings row, or `false` for a line that
/// is not one. The reading lives with the wire's type (`ipc::setting_from_row`)
/// so that the test of the grammar is a test of the wire; this is the name the
/// three places rows are read under.
bool describe_row(const std::string& line, ipc::Setting& out)
{
    return ipc::setting_from_row(line, out);
}

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

std::vector<std::string> split_stages(const std::string& value);

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
        refused_.resize(files_.size());
    }

    ISource* at(std::size_t index) override
    {
        IMedia* media = at_media(index);
        return media != nullptr ? media->audio() : nullptr;
    }
    std::size_t size() const override
    {
        const std::lock_guard lock{lock_};
        return files_.size();
    }
    bool silent(std::size_t index) const override
    {
        const std::lock_guard lock{lock_};
        return index < opened_.size() && opened_[index] && opened_[index]->audio() == nullptr;
    }

    /// What an entry is, opened if it has not been.
    enum class Kind {
        /// A track with audio: a queue's business.
        audio,
        /// A picture with no audio: a run of its own, on the video engine's clock.
        silent,
        /// It would not open, and the log says why. Walked past.
        unreadable,
        /// Past the end.
        end,
    };
    Kind kind_at(std::size_t index)
    {
        if (index >= size()) {
            return Kind::end;
        }
        IMedia* media = at_media(index);
        if (media == nullptr) {
            return Kind::unreadable;
        }
        return media->audio() != nullptr ? Kind::audio : Kind::silent;
    }

    /// The entry before `index` that is not known to be unreadable, or `index`
    /// itself when there is none. What *previous* means from a picture playing
    /// alone near its start.
    [[nodiscard]] std::size_t previous_from(std::size_t index) const
    {
        const std::lock_guard lock{lock_};
        std::size_t at = index;
        while (at > 0) {
            --at;
            if (refused_[at].empty()) {
                return at;
            }
        }
        return index;
    }

    /// **Moves an entry the queue has not reached.** The decode thread opens
    /// entries lazily through `at`, so an entry past the decoder's index is a
    /// path and a null slot and can change places freely; whether an entry is
    /// past it is `Player::move_entry`'s to decide, since only it sees the
    /// queue. The lock is against `at`, which is the decode thread's.
    void move(std::size_t from, std::size_t to)
    {
        const std::lock_guard lock{lock_};
        if (from >= files_.size() || to >= files_.size() || from == to) {
            return;
        }
        const auto shift = [](auto& items, std::size_t a, std::size_t b) {
            if (a < b) {
                std::rotate(items.begin() + static_cast<std::ptrdiff_t>(a),
                            items.begin() + static_cast<std::ptrdiff_t>(a) + 1,
                            items.begin() + static_cast<std::ptrdiff_t>(b) + 1);
            } else {
                std::rotate(items.begin() + static_cast<std::ptrdiff_t>(b),
                            items.begin() + static_cast<std::ptrdiff_t>(a),
                            items.begin() + static_cast<std::ptrdiff_t>(a) + 1);
            }
        };
        shift(files_, from, to);
        shift(opened_, from, to);
        shift(refused_, from, to);
    }

    /// The whole file, for the caller that wants the picture too.
    ///
    /// `at` is `IPlaylist`'s and hands back the audio because that is what a
    /// `Queue` reads; this is the same object with the rest of it still
    /// attached, and both point into one demuxer with one position (§4).
    IMedia* at_media(std::size_t index)
    {
        const std::lock_guard lock{lock_};
        if (index >= files_.size()) {
            return nullptr;
        }
        if (opened_[index]) {
            return opened_[index].get();
        }
        if (!refused_[index].empty()) {
            // Asked again -- a queue walks past it on every pass -- and the
            // answer has not changed. Once in the log is the right number.
            return nullptr;
        }
        std::string why;
        auto media = host_->open_media(files_[index], why);
        if (!media) {
            // Recorded rather than fatal: a playlist that silently plays four
            // of its five entries is worse than one that says which it skipped.
            // The queue walks past it, and the reason is kept for the run that
            // finds nothing to play at all -- see `refusals`.
            refused_[index] = why.empty() ? "no module recognised it" : why;
            host_->log("skipping " + files_[index] + ": " + refused_[index]);
            return nullptr;
        }
        opened_[index] = std::move(media);
        return opened_[index].get();
    }

    /// What would not open, in the words of whoever refused it, for the run
    /// that found nothing to play: the last entry refused, named the way a
    /// person names a file, and how many there were. Empty when nothing was.
    [[nodiscard]] std::string refusals() const
    {
        const std::lock_guard lock{lock_};
        std::size_t count = 0;
        std::size_t last = 0;
        for (std::size_t i = 0; i < refused_.size(); ++i) {
            if (!refused_[i].empty()) {
                ++count;
                last = i;
            }
        }
        if (count == 0) {
            return {};
        }
        const std::string& path = files_[last];
        const std::size_t slash = path.find_last_of("\\/");
        std::string line = (slash == std::string::npos ? path : path.substr(slash + 1)) +
                           ": " + refused_[last];
        if (count > 1) {
            line = std::to_string(count) + " entries would not open; the last, " + line;
        }
        return line;
    }

    // By value, both: a reference into a vector that `move` can shift is a
    // reference that may not survive the call it was returned from.
    [[nodiscard]] std::string decoder_name(std::size_t index) const
    {
        const std::lock_guard lock{lock_};
        return index < opened_.size() && opened_[index] ? opened_[index]->decoder()
                                                        : std::string{};
    }
    [[nodiscard]] std::string path(std::size_t index) const
    {
        const std::lock_guard lock{lock_};
        return index < files_.size() ? files_[index] : std::string{};
    }

private:
    IEngineHost* host_;
    mutable std::mutex lock_;
    std::vector<std::string> files_;
    std::vector<std::unique_ptr<IMedia>> opened_;
    /// Why entry `i` would not open, once it has been tried; empty until then.
    std::vector<std::string> refused_;
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

bool Player::play_at(std::size_t index, std::string& why)
{
    std::vector<std::string> files;
    {
        const std::lock_guard lock{mutex_};
        if (index >= files_.size()) {
            why = "the playlist has " + std::to_string(files_.size()) + " entries and no " +
                  std::to_string(index + 1) + "th";
            return false;
        }
        files = files_;
    }
    // The same request `play` makes, with a first item that is not the first.
    play(std::move(files), index);
    return true;
}

bool Player::move_entry(std::size_t from, std::size_t to, std::string& why)
{
    const std::lock_guard lock{mutex_};
    if (from >= files_.size() || to >= files_.size()) {
        why = "the playlist has " + std::to_string(files_.size()) + " entries";
        return false;
    }
    if (from == to) {
        return true;
    }
    if (queue_ != nullptr || alone_ != nullptr) {
        // **Only what the decoder has not reached.** A queue records where each
        // track began as it goes past it, and the ring holds what it read
        // ahead; an entry at or before the decoder's index is either playing,
        // already in the ring, or already marked, and moving it would move the
        // ground the run stands on. Past it, an entry is a path in a list. A
        // picture on its own is the one entry it is on.
        const std::size_t read = queue_ != nullptr ? queue_->index() : index_;
        if (from <= read || to <= read) {
            why = "the engine has already read up to entry " + std::to_string(read + 1) +
                  "; entries it has passed cannot be moved while they play";
            return false;
        }
    }
    const std::string moved = files_[from];
    files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(from));
    files_.insert(files_.begin() + static_cast<std::ptrdiff_t>(to), moved);
    if (playlist_ != nullptr) {
        playlist_->move(from, to);
    }
    return true;
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
    if (alone_ != nullptr && alone_->own_clock() != nullptr) {
        // A picture on its own clock: the clock stops, and the loop, which
        // follows it, holds the frame it is on.
        alone_->own_clock()->pause();
        state_ = ipc::State::paused;
        return;
    }
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
    if (alone_ != nullptr && alone_->own_clock() != nullptr) {
        alone_->own_clock()->resume();
        state_ = ipc::State::playing;
        return;
    }
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
    if (alone_ != nullptr && alone_->own_clock() != nullptr) {
        // **The picture on its own clock.** The same clamp, then the file is
        // moved through its own door rather than through a graph: see
        // `VideoPath::seek_alone`, which is `seek_together` with no audio in
        // the middle.
        const std::uint64_t here = alone_->own_clock()->position();
        const std::int64_t wanted = relative ? static_cast<std::int64_t>(here) + frames : frames;
        const auto to = static_cast<std::uint64_t>(std::max<std::int64_t>(0, wanted));
        IMedia* media = alone_media_;
        std::string why;
        return alone_->seek_alone(static_cast<double>(to) / VideoPath::k_own_rate,
                                  [media](double seconds) { return media->seek_picture(seconds); },
                                  why);
    }
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
    // Held for the whole call, as `seek` holds it: the graph may not be
    // destroyed underneath this, and the engine thread clears these pointers
    // under the same lock before it destroys anything.
    const std::lock_guard lock{mutex_};
    if (alone_ != nullptr) {
        // **A picture on its own is one entry, so next is the entry after
        // it.** Its run ends and `play_request` walks on; there is no ring to
        // throw away and no listener to count from.
        step_wanted_.store(1, std::memory_order_release);
        return;
    }
    if (queue_ == nullptr) {
        return;
    }
    if (graph_a_ == nullptr && graph_b_ == nullptr) {
        // Nothing is running, so there is no ring to throw away and no
        // listener to count from: the decoder's own track is the only one.
        queue_->skip();
        return;
    }
    // **From where the listener is, and the ring goes with it.** `skip` asked
    // the decoder to abandon its track, which with a deep ring can be a track
    // the listener has not reached, and left the ring alone, so the rest of
    // what was playing played out first -- a button that seemed to do nothing
    // for most of a second. A seek to the device's own position, with the
    // queue told to land on the next track, does what the button means: the
    // ring is reset, silence is written while the next track is decoded to
    // the floor, and it starts the moment it is there. `Queue::request_next`
    // says why that is a seek and not a skip.
    const std::uint64_t here = graph_a_ != nullptr ? graph_a_->position_frames()
                                                   : graph_b_->position_frames();
    queue_->request_next();
    const bool moved = graph_a_ != nullptr ? graph_a_->seek(here) : graph_b_->seek(here);
    if (!moved) {
        // A source that cannot seek. The old way is what is left.
        queue_->cancel_next();
        queue_->skip();
    }
}

void Player::previous()
{
    const std::lock_guard lock{mutex_};
    if (alone_ != nullptr && alone_->own_clock() != nullptr) {
        // The rule below, for a picture on its own: its start, unless you have
        // only just got here, in which case the entry before it.
        const std::uint64_t here = alone_->own_clock()->position();
        if (here > 3ull * VideoPath::k_own_rate) {
            IMedia* media = alone_media_;
            std::string why;
            (void)alone_->seek_alone(
                0.0, [media](double seconds) { return media->seek_picture(seconds); }, why);
        } else {
            step_wanted_.store(-1, std::memory_order_release);
        }
        return;
    }
    if (queue_ == nullptr) {
        return;
    }
    // What a "previous" button means everywhere: the start of this track,
    // unless you have only just got here, in which case the one before. *This
    // track* is the one being heard, asked at the device's position; the
    // decoder's is up to a ring's depth ahead.
    const std::uint64_t here = graph_a_ != nullptr   ? graph_a_->position_frames()
                               : graph_b_ != nullptr ? graph_b_->position_frames()
                                                     : queue_->item_start();
    const std::uint64_t start = queue_->start_at(here);
    const std::uint64_t grace = queue_->format().sample_rate * 3ull;
    const std::uint64_t target = (here > start + grace || !queue_->has_previous_at(here))
                                     ? start
                                     : queue_->previous_start_at(here);
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
    // **The device's position first, because everything below is asked at
    // it.** Which track is playing and how far into it are questions about
    // what is coming out of the endpoint, not about what the decoder has read:
    // a gapless queue reads ahead by the ring's depth, so the two answers can
    // be several tracks apart.
    s.position = position_;
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
    s.clock_rate = clock_rate_;
    if (alone_ != nullptr && alone_->own_clock() != nullptr) {
        // A picture on its own clock: one entry, its own timeline, counted in
        // `VideoPath::k_own_rate`. No queue to convert through.
        s.position = alone_->own_clock()->position();
        s.item_position = s.position;
        s.length = alone_length_;
    }
    if (queue_ != nullptr) {
        s.length = queue_->length_frames();
        s.index = static_cast<std::uint32_t>(queue_->index_at(s.position));
        s.item_position = s.position - queue_->start_at(s.position);
        // From the playlist rather than from what the run started with: a queue
        // crosses track boundaries without telling the graph, which is the
        // point of it, so the name has to be looked up and not remembered.
        if (s.index < files_.size()) {
            s.track = files_[s.index];
        }
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

namespace {

bool parse_pairs(const std::string& value,
                 std::vector<std::pair<std::string, std::string>>& out);
std::string joined_pairs(const std::vector<std::pair<std::string, std::string>>& pairs);

} // namespace

std::vector<ipc::Setting> Player::settings() const
{
    const std::lock_guard lock{mutex_};
    std::vector<ipc::Setting> out;
    // The fourth field spelled the way a module spells it, so that the
    // player's own rows and a module's are one grammar with one reader.
    const auto row = [&out](const char* key, std::string value, std::string what,
                            const char* spec) {
        ipc::Setting setting{key, std::move(value), std::move(what)};
        (void)ipc::parse_setting_spec(spec, setting);
        out.push_back(std::move(setting));
    };
    row("device", config_.device,
        "part of an endpoint's name, or empty for the default one", "text group=Output");
    row("share", config_.shared ? "shared" : "exclusive",
        "exclusive takes the device and nothing else can make a sound on it",
        "enum:exclusive,shared group=Output");
    row("path", path_policy_name(config_.path),
        "bitexact, exact, auto or processed -- what may happen to the samples",
        "enum:bitexact,exact,auto,processed group=Path");
    row("dsp", joined(config_.dsp, '|'),
        "stages in the order they run, separated by |; each `name` or "
        "`name:key=value,key=value`",
        "text group=Path");
    row("video_dsp", joined(config_.video_dsp, '|'),
        "the same for the picture: stages in linear light inside the presenter",
        "text group=Path");
    row("presenter", joined_pairs(config_.presenter),
        "what was set on the presenter, `key=value,key=value`, said again at every "
        "open -- the tone mapper, the gamut, the siting; never its size, which is "
        "the window's",
        "text group=Path");
    row("gain", std::to_string(config_.conversion.gain),
        "linear, not decibels. Only on the processed path",
        "number step=0.05 group=Conversion");
    row("dither", dither_kind_name(config_.conversion.dither),
        "none, rectangular, triangular, highpass or gaussian, when a container shrinks",
        "enum:none,rectangular,triangular,highpass,gaussian group=Conversion");
    // The value is what was typed, because this list is also what gets written
    // to the settings file and a file this program cannot read back is not a
    // settings file. What it resolved to goes in the description, where it is
    // just as visible and cannot be mistaken for an input.
    row("shaping", config_.shaping_spec,
        "0-9 for a binomial order, `shibata[:N]`, or a named curve -- currently " +
            noise_shaping_describe(config_.conversion.shaping,
                                   wire_.sample_rate != 0 ? wire_.sample_rate : 44100),
        "text group=Conversion");
    row("dither_seed", std::to_string(config_.conversion.seed),
        "so two runs of one file produce the same bytes", "int min=0 group=Conversion");
    row("ring_periods", std::to_string(config_.buffering.ring_periods),
        "ring capacity in device periods. Generous by default, because the "
        "worst stall in a file is not knowable before opening it",
        "int min=1 group=Buffering");
    row("prefill_periods", std::to_string(config_.buffering.prefill_periods),
        "how much of the ring is filled before the device starts and before a "
        "seek resumes. Not the whole ring",
        "int min=1 group=Buffering");
    row("wait_timeout", std::to_string(config_.buffering.wait_timeout_ms),
        "how long the render thread waits for a device before calling it gone",
        "int min=0 unit=ms group=Buffering");
    row("recover", config_.recover ? "1" : "0",
        "rebuild onto an endpoint that comes back, instead of ending the run",
        "bool group=Recovery");
    row("recover_timeout", std::to_string(config_.recover_timeout),
        "seconds to wait for one", "int min=0 unit=s group=Recovery when=recover=1");
    return out;
}

bool Player::set(const std::string& key, const std::string& value, std::string& why)
{
    bool rebuild = false;
    bool apply_presenter = false;
    std::vector<std::pair<std::string, std::string>> presenter_was;
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
            config_.dsp = split_stages(value);
            rebuild = true;
        } else if (key == "video_dsp") {
            // **A rebuild for the same reason the audio chain is one.** The
            // chain is opened when the picture is, on the presenter's device,
            // and a stage inserted underneath a display loop that is inside
            // `process` is a data race rather than a setting.
            config_.video_dsp = split_stages(value);
            rebuild = true;
        } else if (key == "presenter") {
            // **Kept, and applied to the picture that is open.** What the
            // settings file replays at the start, and what `set_node` writes
            // one key at a time: the same list, read the same way.
            std::vector<std::pair<std::string, std::string>> pairs;
            if (!parse_pairs(value, pairs)) {
                why = "presenter is `key=value,key=value`";
                return false;
            }
            presenter_was = config_.presenter;
            config_.presenter = std::move(pairs);
            apply_presenter = true;
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
            // Somebody said. From here a measured profile does not overrule it.
            config_.ring_periods_chosen = true;
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
    if (apply_presenter) {
        // **Outside the lock**, because a presenter's setting waits for the
        // display loop's turn. A refusal puts the list back and says why: the
        // presenter is the one that knows what its keys mean.
        const std::shared_ptr<VideoPath> video = picture();
        std::vector<std::pair<std::string, std::string>> pairs;
        {
            const std::lock_guard lock{mutex_};
            pairs = config_.presenter;
        }
        for (const auto& [k, v] : pairs) {
            if (video != nullptr && !video->set_presenter(k, v, why)) {
                const std::lock_guard lock{mutex_};
                config_.presenter = presenter_was;
                return false;
            }
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// The engine thread
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// The shape, for a shell that draws it (§10)
// --------------------------------------------------------------------------

namespace {

/// A `--dsp` spec split into the module and its settings: `resample:rate=48000`
/// is `dsp_resample` and one pair. **One place**, because `build_chain` reads
/// the same text and two readers of one grammar are two grammars.
struct StageSpec {
    std::string id;                                        // with the dsp_ prefix
    std::vector<std::pair<std::string, std::string>> settings;
};

/// A chain, as one string: stages separated by `|`, each `name` or
/// `name:key=value,key=value`.
///
/// **One separator was doing two jobs.** Stages were joined with a comma and
/// a stage's own settings are separated by commas, so `mix:channels=2,
/// normalise=energy` came back through `set dsp` -- and through the settings
/// file `save` writes -- as a stage called `mix` and a second one called
/// `normalise=energy`. Nobody had hit it: the CLI takes one `--dsp` per stage
/// and never joins, and `set_node` writes a stage's keys into its own element
/// of `config_.dsp` where no split happens. The shell's canvas is the first
/// thing that rewrites the whole string, and it would have hit it at once.
///
/// `|`, because it cannot be in a Windows path (a `file=` value is a path) and
/// because `;` and `#` start a comment in the settings file. The old form is
/// still read: a comma-separated piece with an `=` before any `:` is a setting
/// and belongs to the stage before it, and a stage name never contains `=`.
/// `key=value,key=value`, which is a stage's own tail without the stage:
/// what the `presenter` row is spelled in.
bool parse_pairs(const std::string& value,
                 std::vector<std::pair<std::string, std::string>>& out)
{
    out.clear();
    for (const std::string& piece : split(value, ',')) {
        const std::size_t equals = piece.find('=');
        if (equals == std::string::npos || equals == 0) {
            return false;
        }
        out.emplace_back(piece.substr(0, equals), piece.substr(equals + 1));
    }
    return true;
}

std::string joined_pairs(const std::vector<std::pair<std::string, std::string>>& pairs)
{
    std::string out;
    for (const auto& [key, value] : pairs) {
        out += (out.empty() ? "" : ",") + key + "=" + value;
    }
    return out;
}

std::vector<std::string> split_stages(const std::string& value)
{
    std::vector<std::string> out;
    for (const std::string& group : split(value, '|')) {
        for (const std::string& piece : split(group, ',')) {
            const std::size_t equals = piece.find('=');
            const std::size_t colon = piece.find(':');
            const bool setting = equals != std::string::npos &&
                                 (colon == std::string::npos || equals < colon);
            if (setting && !out.empty()) {
                out.back() += "," + piece;
            } else {
                out.push_back(piece);
            }
        }
    }
    return out;
}

StageSpec parse_stage(const std::string& spec)
{
    StageSpec out;
    const std::size_t colon = spec.find(':');
    out.id = spec.substr(0, colon);
    if (out.id.rfind("dsp_", 0) != 0) {
        out.id = "dsp_" + out.id;
    }
    if (colon == std::string::npos) {
        return out;
    }
    for (const std::string& one : split(spec.substr(colon + 1), ',')) {
        const std::size_t equals = one.find('=');
        if (equals != std::string::npos) {
            out.settings.emplace_back(one.substr(0, equals), one.substr(equals + 1));
        }
    }
    return out;
}

/// The spec a stage's settings make, in the form `config_.dsp` holds.
std::string write_stage(const StageSpec& stage)
{
    std::string out = stage.id;
    for (std::size_t i = 0; i < stage.settings.size(); ++i) {
        out += (i == 0 ? ':' : ',');
        out += stage.settings[i].first + "=" + stage.settings[i].second;
    }
    return out;
}

} // namespace

ipc::Graph Player::graph() const
{
    PlayerConfig config;
    bool processing = false;
    std::string decoder;
    std::string device;
    std::string track;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
        processing = processed_;
        decoder = decoder_;
        device = device_;
        track = track_;
    }

    ipc::Graph out;
    const auto node = [&out](const char* id, ipc::NodeKind kind, std::string module,
                             std::string name, std::uint32_t flags) {
        out.nodes.push_back(ipc::Node{id, static_cast<std::uint32_t>(kind),
                                      std::move(module), std::move(name), flags});
    };
    const auto edge = [&out](std::string from, std::string to) {
        out.edges.push_back(ipc::Edge{std::move(from), std::move(to)});
    };

    node("source", ipc::NodeKind::source, decoder,
         track.empty() ? "the file" : track, 0);

    // **The chain is what a person assembled**, so it is what a canvas may
    // rearrange. Everything else on this line is decided by §5 and §6 and is
    // there whether anybody wants it or not.
    std::string previous = "source";
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < config.dsp.size(); ++i) {
        const StageSpec stage = parse_stage(config.dsp[i]);
        const std::string id = "dsp." + std::to_string(i);
        node(id.c_str(), ipc::NodeKind::dsp, stage.id, stage.id,
             ipc::MP_NODE_REMOVABLE | ipc::MP_NODE_SETTABLE);
        edge(previous, id);
        previous = id;
        ids.push_back(id);
    }

    // **Path B's converter, and only on Path B.** §5: Path A is a `memcpy` or a
    // container repack with nothing insertable in it, and drawing a node there
    // would be drawing a stage that does not exist. A chain forces Path B, so a
    // canvas with stages on it always has this.
    const bool converting =
        processing || !config.dsp.empty() || config.path == PathPolicy::processed;
    if (converting) {
        node("convert", ipc::NodeKind::convert, {},
             "the f64 bus: gain, dither and noise shaping", ipc::MP_NODE_SETTABLE);
        edge(previous, "convert");
        previous = "convert";
    }

    node("sink", ipc::NodeKind::sink, {},
         device.empty() ? "the device" : device, ipc::MP_NODE_SETTABLE);
    edge(previous, "sink");

    // **The picture, and it is a second line rather than a branch.** §4 gives
    // the file one position and §8 gives the run one clock, but the frames and
    // the samples never meet: what joins them is the clock, which is not an
    // edge a canvas should draw as though data flowed along it.
    // **One copy of the shape, taken under the path's gate**, rather than
    // four questions asked while the engine thread may be answering none of
    // them: the module names and the stage list are rewritten by an open.
    const std::shared_ptr<VideoPath> video = picture();
    const VideoPath::Shape shape = video ? video->shape() : VideoPath::Shape{};
    if (shape.opened) {
        node("vsource", ipc::NodeKind::video_source, shape.decoder, "the video decoder",
             ipc::MP_NODE_SETTABLE);
        std::string before = "vsource";
        for (std::size_t i = 0; i < shape.stages.size(); ++i) {
            const std::string id = "vdsp." + std::to_string(i);
            const std::string& module = shape.stages[i];
            node(id.c_str(), ipc::NodeKind::video_stage, module, module,
                 ipc::MP_NODE_REMOVABLE | ipc::MP_NODE_SETTABLE);
            edge(before, id);
            before = id;
        }
        node("presenter", ipc::NodeKind::presenter, shape.presenter,
             "the colour pipeline and the display", ipc::MP_NODE_SETTABLE);
        edge(before, "presenter");
    }
    return out;
}

std::vector<ipc::Setting> Player::node_settings(const std::string& node) const
{
    PlayerConfig config;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
    }

    if (node == "convert" || node == "sink" || node == "source") {
        // **The run's own settings, filtered to the node they belong to.** They
        // are `Player::set` keys either way, so a shell that sets one through a
        // node and a shell that sets it through the settings tree are doing the
        // same thing -- which is what stops the two drifting.
        static const char* const of_convert[] = {"gain", "dither", "shaping",
                                                 "dither_seed", "path"};
        static const char* const of_sink[] = {"device", "share", "ring_periods",
                                              "prefill_periods", "wait_timeout",
                                              "recover", "recover_timeout"};
        const auto wanted = [&](const std::string& key) {
            if (node == "convert") {
                return std::any_of(std::begin(of_convert), std::end(of_convert),
                                   [&](const char* k) { return key == k; });
            }
            if (node == "sink") {
                return std::any_of(std::begin(of_sink), std::end(of_sink),
                                   [&](const char* k) { return key == k; });
            }
            return false;
        };
        std::vector<ipc::Setting> out;
        for (const ipc::Setting& row : settings()) {
            if (wanted(row.key)) {
                out.push_back(row);
            }
        }
        return out;
    }

    if (node == "presenter") {
        // The colour pipeline's own answers (§9), which is what a shell shows
        // beside the picture: what the display turned out to be, what the
        // buffer holds, which tone mapper is in the path.
        const std::shared_ptr<VideoPath> video = picture();
        if (video == nullptr) {
            return {};
        }
        std::vector<ipc::Setting> out;
        for (const std::string& line : video->presenter_describe()) {
            ipc::Setting row_out;
            if (describe_row(line, row_out)) {
                out.push_back(std::move(row_out));
            }
        }
        return out;
    }
    if (node == "vsource") {
        // **What the picture is actually doing.** Every one of these is a
        // measurement rather than a setting, and until there was a shell there
        // was nowhere to read them from a running engine at all: `show` prints
        // them when its window closes, which is a tool with a window, and the
        // engine that draws into a composition surface had no equivalent. It
        // is the difference between *the picture is connected* and *the
        // picture is being drawn*, and that difference is exactly what a black
        // window does not tell you.
        const std::shared_ptr<VideoPath> video = picture();
        if (video == nullptr || !video->opened()) {
            return {};
        }
        const auto row = [](const char* key, const std::string& value,
                            const char* what) {
            return ipc::Setting{key, value, std::string{what} + " (read only)", true};
        };
        const VideoGraph::Stats frames = video->graph_stats();
        std::vector<ipc::Setting> out;
        out.push_back(row("module", video->modules().decoder, "what decodes it"));
        out.push_back(row("decoded", std::to_string(frames.decoded),
                          "frames the decoder produced"));
        out.push_back(row("shown", std::to_string(frames.shown),
                          "frames the presenter was given"));
        out.push_back(row("dropped", std::to_string(frames.dropped),
                          "frames let go because their time had passed"));
        out.push_back(row("preroll", std::to_string(frames.preroll),
                          "frames decoded on the way to a seek's target and let go"));
        if (video->running()) {
            const DisplayLoop::Stats loop = video->loop_stats();
            out.push_back(row("turns", std::to_string(loop.turns),
                              "times the display said a frame could be drawn"));
            out.push_back(row("no_clock", std::to_string(loop.without_clock),
                              "of those, turns with no clock to read"));
            char measured[64];
            std::snprintf(measured, sizeof measured, "%.3f ms",
                          loop.refresh_seconds * 1000.0);
            out.push_back(row("refresh", measured, "the display's, as measured here"));
        } else {
            out.push_back(row("turns", "0", "the display loop is not running"));
        }
        return out;
    }
    if (node.rfind("vdsp.", 0) == 0) {
        // **Asked of the live stage, under the display loop's hold.** Unlike an
        // audio stage there is no opening one for the question: a video stage
        // opens on the presenter's device (§9.8.1) and there is exactly one of
        // those. The hold is `set_size`'s, for the same reason.
        const std::shared_ptr<VideoPath> video = picture();
        if (video == nullptr) {
            return {};
        }
        const auto index = static_cast<std::size_t>(std::atoi(node.c_str() + 5));
        std::vector<ipc::Setting> out;
        for (const std::string& line : video->stage_describe(index)) {
            ipc::Setting row_out;
            if (describe_row(line, row_out)) {
                out.push_back(std::move(row_out));
            }
        }
        return out;
    }
    if (node.rfind("dsp.", 0) != 0) {
        return {};
    }
    const std::size_t index = static_cast<std::size_t>(std::atoi(node.c_str() + 4));
    if (index >= config.dsp.size()) {
        return {};
    }

    // **Asked of a stage opened for the question, not of the one that is
    // playing.** A `describe` on the live stage would be an IPC thread calling
    // into a module the render thread is inside; a fresh one with the same
    // settings resolves the same way and answers with every key it has rather
    // than only the ones somebody has already set.
    const StageSpec spec = parse_stage(config.dsp[index]);
    const MpDspVtbl* vtbl = host_->dsp(spec.id);
    if (vtbl == nullptr) {
        return {};
    }
    DspChain asking;
    asking.add(*vtbl, spec.id);
    DspStage& stage = asking.at(0);
    if (!stage.open()) {
        return {};
    }
    for (const auto& [key, value] : spec.settings) {
        (void)stage.set(key, value);
    }

    std::vector<ipc::Setting> out;
    for (const std::string& line : stage.describe()) {
        ipc::Setting row_out;
        if (describe_row(line, row_out)) {
            out.push_back(std::move(row_out));
        }
    }
    return out;
}

bool Player::set_node(const std::string& node, const std::string& key,
                      const std::string& value, std::string& why)
{
    if (node == "convert" || node == "sink" || node == "source") {
        // The same keys under a different name, so `set` decides what they mean
        // and there is one place that does.
        return set(key, value, why);
    }
    if (node == "presenter") {
        // The presenter's own keys, under the display loop's hold -- the same
        // route a resize takes, and for the same reason: one graphics context
        // is not two threads' to share.
        const std::shared_ptr<VideoPath> video = picture();
        if (video == nullptr) {
            why = "nothing is showing a picture, so there is no presenter to set";
            return false;
        }
        // **Remembered before it is applied, and put back if the apply
        // fails.** The picture is built again at every track boundary, so a
        // size that lived only in the presenter would be forgotten a second
        // into a playlist of one-second files -- and the apply below takes the
        // display loop's hold, which can wait, so a boundary crossed *during*
        // it would open the next picture from a value not written yet. That is
        // the order this is in: write, apply, put back.
        std::uint32_t was_width = 0;
        std::uint32_t was_height = 0;
        if (key == "size") {
            const std::lock_guard lock{mutex_};
            was_width = asked_width_;
            was_height = asked_height_;
            asked_width_ = 0;
            asked_height_ = 0;
            if (value != "native") {
                char* end = nullptr;
                const unsigned long w = std::strtoul(value.c_str(), &end, 10);
                if (end != nullptr && (*end == 'x' || *end == 'X')) {
                    const unsigned long h = std::strtoul(end + 1, &end, 10);
                    if (end != nullptr && *end == '\0') {
                        asked_width_ = static_cast<std::uint32_t>(w);
                        asked_height_ = static_cast<std::uint32_t>(h);
                    }
                }
            }
        }
        if (!video->set_presenter(key, value, why)) {
            if (key == "size") {
                const std::lock_guard lock{mutex_};
                asked_width_ = was_width;
                asked_height_ = was_height;
            }
            return false;
        }
        // **Kept, so that the next open and the settings file both say it.**
        // The size and the display are the window's and are carried on their
        // own; the surface is decided before the first configure and is not a
        // person's to set twice.
        if (key != "size" && key != "display" && key != "surface") {
            const std::lock_guard lock{mutex_};
            bool replaced = false;
            for (auto& pair : config_.presenter) {
                if (pair.first == key) {
                    pair.second = value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                config_.presenter.emplace_back(key, value);
            }
        }
        return true;
    }
    if (node.rfind("vdsp.", 0) == 0) {
        // **Applied to the stage rather than rebuilt into it**, which is the
        // one place the two chains differ: a video stage's `set` happens under
        // the loop's hold and costs a held frame, where an audio stage's would
        // have to reach a render thread with a 3 ms deadline and so rebuilds.
        // The spec is updated too, so `save` writes down what is running.
        const std::shared_ptr<VideoPath> video = picture();
        if (video == nullptr) {
            why = "nothing is showing a picture, so there is no chain to set";
            return false;
        }
        const auto index = static_cast<std::size_t>(std::atoi(node.c_str() + 5));
        if (!video->set_stage(index, key, value, why)) {
            return false;
        }
        // **The spec the stage was opened from**, which is not the stage's
        // index when one before it would not open.
        const std::size_t which = video->stage_spec_index(index);
        const std::lock_guard lock{mutex_};
        if (which < config_.video_dsp.size()) {
            std::string& spec = config_.video_dsp[which];
            const std::size_t colon = spec.find(':');
            std::string kept = spec.substr(0, colon);
            std::string rest = colon == std::string::npos ? std::string{}
                                                          : spec.substr(colon + 1);
            // Whatever was there for this key goes; the new one is appended.
            std::string built;
            for (const std::string& one : split(rest, ',')) {
                if (one.rfind(key + "=", 0) != 0 && !one.empty()) {
                    built += (built.empty() ? "" : ",") + one;
                }
            }
            built += (built.empty() ? "" : ",") + key + "=" + value;
            spec = kept + ":" + built;
        }
        return true;
    }
    if (node == "vsource") {
        // Its rows are measurements -- the decoder's name, what it produced,
        // what was dropped -- and a canvas that opens them sees that; a set
        // is answered in words rather than with a node that does not exist.
        why = "vsource has nothing to set: its rows are what the decoder did";
        return false;
    }
    if (node.rfind("dsp.", 0) != 0) {
        why = "there is no node called `" + node + "`";
        return false;
    }

    std::string spec;
    {
        const std::lock_guard lock{mutex_};
        const std::size_t index = static_cast<std::size_t>(std::atoi(node.c_str() + 4));
        if (index >= config_.dsp.size()) {
            why = "there is no node called `" + node + "`";
            return false;
        }
        StageSpec stage = parse_stage(config_.dsp[index]);
        bool replaced = false;
        for (auto& pair : stage.settings) {
            if (pair.first == key) {
                pair.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            stage.settings.emplace_back(key, value);
        }
        config_.dsp[index] = write_stage(stage);
        spec = config_.dsp[index];
    }

    // **Checked by building it**, which is the only honest check: a stage
    // decides what its own settings mean, and asking it is asking the thing
    // that knows. A refusal puts the chain back before anything is rebuilt.
    DspChain trying;
    if (!build_chain(trying, why)) {
        const std::lock_guard lock{mutex_};
        StageSpec stage = parse_stage(spec);
        stage.settings.erase(std::remove_if(stage.settings.begin(), stage.settings.end(),
                                            [&](const auto& p) { return p.first == key; }),
                             stage.settings.end());
        const std::size_t index = static_cast<std::size_t>(std::atoi(node.c_str() + 4));
        if (index < config_.dsp.size()) {
            config_.dsp[index] = write_stage(stage);
        }
        return false;
    }

    rebuild_wanted_.store(true, std::memory_order_release);
    return true;
}

std::vector<ipc::ModuleRow> Player::modules() const
{
    return host_->modules();
}

std::uint64_t Player::surface() const
{
    const std::shared_ptr<VideoPath> video = picture();
    return video ? video->surface() : 0;
}

void Player::use_profile(Profile profile)
{
    const std::lock_guard lock{mutex_};
    config_.profile = std::move(profile);
}

bool Player::set_display(bool known, const VideoPath::DisplayIs& display, std::string& why)
{
    {
        const std::lock_guard lock{mutex_};
        display_known_ = known;
        display_ = display;
    }
    // **Applied now as well as remembered.** A window that crossed a monitor
    // did so while something was playing, and a message that only took effect
    // on the next track would leave the picture graded for the display it left.
    //
    // The reference is taken under the mutex and held for the call, so the
    // engine thread crossing a track boundary replaces its own and does not
    // destroy this one.
    const std::shared_ptr<VideoPath> video = picture();
    if (!video || !video->opened()) {
        return true;
    }
    return known ? video->set_display(display, why) : video->probe_display(why);
}

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
        Sweeping sweep;
        bool measuring = false;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] {
                return quit_.load(std::memory_order_acquire) || !requests_.empty() ||
                       !sweeps_.empty();
            });
            if (quit_.load(std::memory_order_acquire)) {
                return;
            }
            // **A calibration first, because it takes the machine over.** A
            // playlist asked for while one is queued is a playlist that waits;
            // the other order would start a run and then stop it, which is a
            // device opened and closed for nothing.
            if (!sweeps_.empty()) {
                sweep = std::move(sweeps_.front());
                sweeps_.pop_front();
                measuring = true;
            } else {
                request = std::move(requests_.front());
                requests_.pop_front();
            }
        }
        // Consumed here rather than in `play`: whatever asked for this run also
        // asked the previous one to end, and that flag has done its work.
        stop_wanted_.store(false, std::memory_order_release);
        if (measuring) {
            run_sweep(sweep);
        } else {
            play_request(request);
        }
    }
}

// --------------------------------------------------------------------------
// Measuring this machine (§9.8.2)
// --------------------------------------------------------------------------

void Player::calibrate(std::vector<std::string> files, CalibrationPlan plan)
{
    {
        const std::lock_guard lock{mutex_};
        sweeps_.push_back(Sweeping{std::move(files), plan});
    }
    // What is playing stops, so that the measurement is of a machine playing
    // the file under test and nothing else.
    stop_wanted_.store(true, std::memory_order_release);
    wake_.notify_all();
}

std::string Player::profile_text() const
{
    const std::lock_guard lock{mutex_};
    return write_profile(config_.profile);
}

void Player::run_sweep(const Sweeping& sweep)
{
    if (sweep.files.empty()) {
        note("a calibration with no files in it has nothing to measure");
        set_state(ipc::State::stopped);
        return;
    }
    note("measuring " + std::to_string(sweep.files.size()) +
         (sweep.files.size() == 1 ? " file" : " files") +
         "; this cannot run faster than the material");

    const CalibrationReport report = mp::calibrate(*this, sweep.files, sweep.plan);

    for (const std::string& skipped : report.skipped) {
        note("skipped " + skipped);
    }
    note("measured " + std::to_string(report.profile.measured.size()) +
         (report.profile.measured.size() == 1 ? " class of stream in "
                                              : " classes of stream in ") +
         std::to_string(report.runs) + (report.runs == 1 ? " run" : " runs"));
    {
        const std::lock_guard lock{mutex_};
        // **Applied as well as reported.** A measurement this machine made
        // about itself is the answer to the question the default is a guess at,
        // and a shell that had to hand it back would be a shell that could
        // forget to.
        config_.profile = report.profile;
    }
    set_state(ipc::State::stopped);
}

void Player::say(const std::string& line)
{
    note(line);
}

bool Player::inspect(const std::string& file, StreamShape& shape, double& duration,
                     std::string& why)
{
    auto media = host_->open_media(file, why);
    if (!media) {
        return false;
    }
    if (media->video() == nullptr) {
        // **Not a failure of the calibration.** The profile is keyed on a class
        // of video stream, so a file with none has no class to be measured
        // against; it is skipped and named.
        why = "no video in it, so there is no class of stream to measure";
        return false;
    }
    const IMedia::Picture shot = media->picture();
    shape = StreamShape{shot.codec, shot.info.width, shot.info.height, shot.info.fps_num,
                        shot.info.fps_den};

    const ISource* audio = media->audio();
    if (audio == nullptr) {
        // A picture with no sound has no ring under it to measure.
        why = "no audio in it, so there is no ring to measure";
        return false;
    }
    const Format& format = audio->format();
    duration = format.sample_rate != 0
                   ? static_cast<double>(audio->length_frames()) /
                         static_cast<double>(format.sample_rate)
                   : 0.0;
    return true;
}

template <typename Graph>
void Player::measure(Graph& graph, const CalibrationRun& run, RunResult& result)
{
    if (graph.start() != MP_OK) {
        return;
    }
    // **Named, not a temporary.** `DisplayLoop` keeps a reference to this for
    // as long as its thread turns, and a temporary would be gone at the end of
    // the call that handed it over.
    GraphClock<Graph> audible{graph};
    start_video(&audible);

    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::microseconds{
                           static_cast<std::int64_t>(run.seconds * 1'000'000.0)};
    while (graph.running() && std::chrono::steady_clock::now() < until) {
        if (quit_.load(std::memory_order_acquire) ||
            stop_wanted_.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(k_poll);
    }

    stop_video();
    const auto stats = graph.stats();
    graph.stop();

    result.ok = true;
    result.underruns = stats.underruns;
    result.low_water_bytes = stats.low_water_bytes;
    result.ring_bytes = stats.ring_bytes;
    if (video_) {
        result.frames_dropped = video_->graph_stats().dropped;
    }
}

bool Player::play(const CalibrationRun& run, RunResult& result, std::string& why)
{
    result = RunResult{};

    PlayerConfig config;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
    }

    auto media = host_->open_media(run.file, why);
    if (!media) {
        return false;
    }
    ISource* audio = media->audio();
    if (audio == nullptr) {
        why = "no audio in it, so there is no ring to measure";
        return false;
    }

    std::string device;
    Sink sink = host_->open_sink(config.device, config.shared, device, why);
    if (!sink) {
        return false;
    }

    // **No chain.** A calibration measures the path the profile is keyed on,
    // and a stage somebody added is a different path -- measuring with it in
    // would write a number down that stops being true the moment it is removed.
    DspChain chain;
    Wired wired;
    if (!wire_up(sink, audio->format(), chain, config.path,
                 config.conversion.gain != 1.0, wired, why)) {
        if (!wired.negotiated.ok) {
            why = "the device would take none of " +
                  std::to_string(wired.negotiated.tried) + " candidate formats for " +
                  describe(wired.offered);
        }
        return false;
    }

    // **Seeked before anything is started**, so there is nothing to hold still:
    // the source is not being read yet, and moving the router now is what
    // clears the video's queue as well (§4).
    if (run.at_seconds > 0.0 && audio->seekable()) {
        const auto frame = static_cast<std::uint64_t>(
            run.at_seconds * audio->format().sample_rate);
        (void)audio->seek(frame);
    }

    PassthroughConfig ring = config.buffering;
    if (run.ring_periods != 0) {
        ring.ring_periods = run.ring_periods;
    }

    open_video(media.get(), run.decoder_threads);
    {
        const std::lock_guard lock{mutex_};
        track_ = run.file;
        decoder_ = media->decoder();
        device_ = device;
        source_ = audio->format();
        clock_rate_ = audio->format().sample_rate;
        wire_ = wired.negotiated.accepted;
        fidelity_ = static_cast<std::uint32_t>(wired.negotiated.fidelity);
        processed_ = wired.processed;
        error_.clear();
        state_ = ipc::State::playing;
    }

    try {
        if (wired.processed) {
            ProcessedGraph graph{*audio,       sink,    wired.negotiated.accepted,
                                 wired.period_frames,  config.conversion,
                                 nullptr,              ring};
            measure(graph, run, result);
        } else {
            PassthroughGraph graph{*audio,      sink, wired.negotiated.accepted,
                                   wired.period_frames, wired.negotiated.fidelity,
                                   nullptr,             ring};
            measure(graph, run, result);
        }
    } catch (const std::bad_alloc&) {
        // A ring the machine cannot allocate is a run that failed, not a
        // process that ended. The sweep reads that as *do not go larger*.
        why = "not enough memory for a ring of " + std::to_string(ring.ring_periods) +
              " periods";
        forget_video();
        return false;
    }
    forget_video();

    // The device's own numbers, because milliseconds is what a profile stores
    // and periods do not survive being written down.
    result.period_frames = wired.period_frames;
    result.sample_rate = wired.negotiated.accepted.sample_rate;
    result.frame_bytes = frame_bytes(wired.negotiated.accepted);
    return result.ok;
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
    bool played = false;
    step_wanted_.store(0, std::memory_order_release);

    while (!quit_.load(std::memory_order_acquire)) {
        // **What is at `first`, opened if it has not been.** A track with
        // audio is a queue's business, and the queue walks on from it; a
        // picture with no audio is a run of its own, on the video engine's
        // clock; an entry that would not open is walked past, and the playlist
        // has said why; and past the end, the run is over.
        const Playlist::Kind kind = playlist.kind_at(first);
        if (kind == Playlist::Kind::unreadable) {
            ++first;
            continue;
        }
        if (kind == Playlist::Kind::end) {
            std::string why;
            if (!played) {
                // Nothing played at all: in the playlist's words when it has
                // any, since it is what tried each entry, and *no audio track
                // in it* is the sentence a shell should show.
                why = playlist.refusals();
                if (why.empty()) {
                    why = "the playlist has nothing at " + std::to_string(first);
                }
                note(why);
            }
            const std::lock_guard lock{mutex_};
            if (!why.empty()) {
                error_ = why;
            }
            state_ = ipc::State::stopped;
            return;
        }
        if (kind == Playlist::Kind::silent) {
            played = true;
            std::uint64_t position = 0;
            const RunEnd end = play_alone(playlist, first, from, position);
            from = 0;
            if (end == RunEnd::finished || end == RunEnd::advanced) {
                ++first;
                continue;
            }
            if (end == RunEnd::retreated) {
                first = playlist.previous_from(first);
                continue;
            }
            if (end == RunEnd::rebuild) {
                from = position;
                continue;
            }
            // Stopped, or it could not start.
            const std::lock_guard lock{mutex_};
            state_ = ipc::State::stopped;
            position_ = position;
            return;
        }

        Queue queue{playlist, first};
        std::string why;
        if (!queue.open(why)) {
            // `kind_at` just opened this entry as audio, so this is a queue
            // refusing the format it found there. Said, rather than looped on.
            note(why);
            const std::lock_guard lock{mutex_};
            error_ = why;
            state_ = ipc::State::stopped;
            return;
        }
        played = true;
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
            if (end == RunEnd::finished && queue.stopped() == QueueStop::format_change) {
                // The one join a queue will not make, and it is the device's
                // gap rather than the player's: exclusive mode cannot change
                // format without stopping.
                //
                // **Read off the queue, because the graph cannot tell.** A
                // queue that stops for the next track's format looks, to the
                // graph draining it, exactly like a playlist that ended: the
                // read returns nothing and the run finishes. This branch used
                // to test for a `RunEnd` nothing produced, so a playlist whose
                // second track had a different rate stopped at the first
                // boundary and said nothing -- found with a 44.1 kHz file
                // followed by a 48 kHz one, and the fixture in the test.
                first = queue.index() + 1;
                note("the next track needs the device reopened: " +
                     describe(queue.next_format()));
                break;
            }
            if (end == RunEnd::finished && queue.stopped() == QueueStop::silent) {
                // The entry in front of the queue has a picture and no audio:
                // the same shape as a format change, and the run above it.
                first = queue.index() + 1;
                note("the next entry has no audio in it, so its picture plays on the "
                     "video engine's own clock");
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
        if (queue.stopped() != QueueStop::format_change &&
            queue.stopped() != QueueStop::silent) {
            const std::lock_guard lock{mutex_};
            state_ = ipc::State::stopped;
            return;
        }
    }
}

Player::RunEnd Player::play_alone(Playlist& playlist, std::size_t index, std::uint64_t from,
                                  std::uint64_t& position)
{
    // **The other shape of a run: a picture, on the video engine's own
    // clock.** No device is opened and no graph is built; the transport talks
    // to `VideoPath::own_clock`, the position is that clock's, and the run ends
    // when the picture does or when somebody moves on. §8's coupling is not
    // involved because there is nothing to couple: one engine, one clock.
    IMedia* media = playlist.at_media(index);
    if (media == nullptr) {
        return RunEnd::failed;
    }
    PlayerConfig config;
    {
        const std::lock_guard lock{mutex_};
        config = config_;
    }
    {
        const std::lock_guard lock{mutex_};
        applied_ = config;
        error_.clear();
        queue_ = nullptr;
        playlist_ = &playlist;
        track_ = playlist.path(index);
        decoder_ = playlist.decoder_name(index);
        device_.clear();
        source_ = Format{};
        wire_ = Format{};
        fidelity_ = 0;
        processed_ = false;
        index_ = static_cast<std::uint32_t>(index);
        clock_rate_ = VideoPath::k_own_rate;
        alone_length_ = media->picture().duration_ms * VideoPath::k_own_rate / 1000;
        state_ = ipc::State::playing;
    }
    note("playing " + playlist.path(index) + " on the picture's own clock (no audio in it)");
    // A flag raised for a run that has not started describes nothing, exactly
    // as `play_run` treats it: read it fresh from here on.
    rebuild_wanted_.store(false, std::memory_order_release);

    open_video(media, 0);
    const std::shared_ptr<VideoPath> video = picture();
    std::string why;
    if (!video || !video->opened()) {
        // `open_video` has said why, once. With no audio either there is
        // nothing left to play, which is the one case that makes it fatal.
        why = "no audio in it, and the picture would not open";
    } else if (!video->start(nullptr, why, 0.0)) {
        why = "the picture will not be shown: " + why;
    }
    if (!why.empty()) {
        note(why);
        forget_video();
        const std::lock_guard lock{mutex_};
        error_ = why;
        return RunEnd::failed;
    }
    if (from != 0) {
        std::string moved;
        (void)video->seek_alone(
            static_cast<double>(from) / VideoPath::k_own_rate,
            [media](double seconds) { return media->seek_picture(seconds); }, moved);
    }
    {
        const std::lock_guard lock{mutex_};
        alone_ = video.get();
        alone_media_ = media;
    }

    RunEnd end = RunEnd::finished;
    while (!video->ended()) {
        if (quit_.load(std::memory_order_acquire) ||
            stop_wanted_.load(std::memory_order_acquire)) {
            end = RunEnd::stopped;
            break;
        }
        if (rebuild_wanted_.exchange(false, std::memory_order_acq_rel)) {
            end = RunEnd::rebuild;
            break;
        }
        const int step = step_wanted_.exchange(0, std::memory_order_acq_rel);
        if (step > 0) {
            end = RunEnd::advanced;
            break;
        }
        if (step < 0) {
            end = RunEnd::retreated;
            break;
        }
        std::this_thread::sleep_for(k_poll);
    }
    position = video->own_clock() != nullptr ? video->own_clock()->position() : 0;
    {
        const std::lock_guard lock{mutex_};
        alone_ = nullptr;
        alone_media_ = nullptr;
        position_ = position;
    }
    {
        // Said, because a picture that ends is either the file running out or
        // something under it giving up, and the counters tell the two apart.
        const VideoGraph::Stats shown = video->graph_stats();
        const DisplayLoop::Stats turns = video->loop().stats();
        note(std::string{"the picture "} +
             (end == RunEnd::finished ? "ended" : "was left") + " at " +
             std::to_string(position) + " ms: " + std::to_string(turns.turns) +
             " turns, decoded " + std::to_string(shown.decoded) + ", shown " +
             std::to_string(shown.shown) + ", dropped " + std::to_string(shown.dropped) +
             ", pre-roll " + std::to_string(shown.preroll));
    }
    stop_video();
    forget_video();
    {
        const std::lock_guard lock{mutex_};
        playlist_ = nullptr;
    }
    return end;
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
        playlist_ = &playlist;
        track_ = playlist.path(queue.index());
        decoder_ = playlist.decoder_name(queue.index());
        device_ = device;
        source_ = source_format;
        clock_rate_ = source_format.sample_rate;
        wire_ = negotiated.accepted;
        fidelity_ = static_cast<std::uint32_t>(negotiated.fidelity);
        processed_ = processing;
        index_ = static_cast<std::uint32_t>(queue.index());
        state_ = ipc::State::playing;
    }
    note("playing " + playlist.path(queue.index()) + " on " + device + " as " +
         describe(negotiated.accepted) + (processing ? " (processed)" : " (bit-exact)"));

    // **§9.8.2's three answers, in the order that respects who said what.** A
    // number somebody typed wins outright. Failing that, a measurement made on
    // this machine for this class of stream. Failing that, the engine's own
    // generous default, which is what the two above are measured against.
    //
    // **Only for a track with a picture in it**, because that is what the
    // profile is keyed on: a class of *video* stream is what makes a decode
    // expensive enough for the ring to matter, and an audio-only track has no
    // class to look up. It is also why the engine could not do this until it
    // had a video path at all.
    if (!config.ring_periods_chosen && !config.profile.measured.empty()) {
        if (IMedia* media = playlist.at_media(queue.index());
            media != nullptr && media->video() != nullptr) {
            const IMedia::Picture shot = media->picture();
            const StreamShape shape{shot.codec, shot.info.width, shot.info.height,
                                    shot.info.fps_num, shot.info.fps_den};
            const std::uint32_t asked =
                ring_for(shape, config.profile, period, negotiated.accepted.sample_rate,
                         config.buffering.ring_periods);
            if (asked != config.buffering.ring_periods) {
                note("profile: " + std::to_string(asked) + " ring periods rather than " +
                     std::to_string(config.buffering.ring_periods) + ", measured here");
                config.buffering.ring_periods = asked;
            }
        }
    }

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
            end = pump(graph, playlist);
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
            end = pump(graph, playlist);
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
    forget_video();
    {
        const std::lock_guard lock{mutex_};
        queue_ = nullptr;
        playlist_ = nullptr;
    }
    return end;
}


void Player::open_video(Playlist& playlist, std::size_t index)
{
    open_video(playlist.at_media(index), 0);
}

void Player::open_video(IMedia* media, std::uint32_t decoder_threads)
{
    if (media == nullptr || media->video() == nullptr) {
        // Most files, and not an error. A track with no picture after one that
        // had ends the picture, which is what closing it here means; the shell
        // is told by the surface going to zero.
        forget_video();
        return;
    }

    // **§9.7.1's headless case**: no window, so the presenter draws into a
    // composition surface a shell composites, and the frame clock is the event
    // that compositor sets. A shell that is not there yet means a picture
    // nobody is looking at, which costs a decode and is what a shell attaching
    // mid-track has to find already running.
    const IMedia::Picture picture = media->picture();
    VideoPath::Config want;
    want.decoder_threads = decoder_threads;
    {
        const std::lock_guard lock{mutex_};
        want.stages = config_.video_dsp;
        want.presenter_settings = config_.presenter;
        // What a shell last asked for, carried across the boundary.
        want.width = asked_width_;
        want.height = asked_height_;
    }
    // **The picture that is already open takes the next track, if it can.**
    // §8 does not rebuild the audio device at a boundary -- not rebuilding it
    // is what gapless is -- and this is the same rule for the picture. It also
    // keeps the composition surface, which is what a shell attached to: a new
    // presenter is a new handle, so a shell would have to detach and attach,
    // and on a playlist of one-second files that is a black frame and then an
    // empty one, every second.
    if (retrack_video(*media, want)) {
        return;
    }

    forget_video();
    auto path = std::make_shared<VideoPath>();
    std::string why;
    bool known = false;
    VideoPath::DisplayIs display;
    {
        const std::lock_guard lock{mutex_};
        known = display_known_;
        display = display_;
    }
    if (!path->open(*host_, nullptr, *media->video(), picture.info, picture.codec,
                    picture.config, picture.config_bytes, want, why)) {
        // **Not fatal, and said once.** A file whose picture will not open is a
        // file that plays, which is exactly what it did before there was a
        // video path at all; refusing the track would be a player that got
        // worse when it gained a feature.
        note("playing without the picture: " + why);
        return;
    }
    // **Before anything is drawn, and after `configure`.** The plan is decided
    // at `configure` from whatever the presenter then believed; saying it here
    // decides it again, once, rather than letting the first frames go up graded
    // for a guess.
    if (known && !path->set_display(display, why)) {
        note("the display the shell named was refused: " + why);
    }
    // What was kept and not taken, said once each: a key a newer presenter
    // module does not know is not a reason to lose the picture.
    for (const std::string& refusal : path->refused_settings()) {
        note(refusal);
    }
    const std::lock_guard lock{mutex_};
    video_ = std::move(path);
    ++generation_;
}

bool Player::retrack_video(IMedia& media, const VideoPath::Config& want)
{
    // The engine thread's own reference: nothing else may be inside the path
    // while the decoder under it is replaced, and `stop_video` has already
    // stopped the loop.
    const std::shared_ptr<VideoPath> keeping = picture();
    if (keeping == nullptr || !keeping->opened()) {
        return false;
    }
    const IMedia::Picture picture_is = media.picture();
    std::string why;
    if (keeping->retrack(*host_, *media.video(), picture_is.info, picture_is.codec,
                         picture_is.config, picture_is.config_bytes, want, why)) {
        return true;
    }
    // **Not an error yet.** A codec this presenter's device will not decode, or
    // a picture it will not take, is a reason to build the whole path again --
    // which may well succeed, because a fresh presenter may choose differently.
    note("the picture is being built again: " + why);
    return false;
}

void Player::start_video(IMediaClock* follow, double origin_seconds)
{
    if (!video_ || !video_->opened()) {
        return;
    }
    // **After the audio graph has started**, when there is one to follow: a
    // picture paced against a clock that is not running is a picture paced
    // against a guess. With nothing to follow, the path starts the video
    // engine's own clock with the loop.
    std::string why;
    if (!video_->start(follow, why, origin_seconds)) {
        note("the picture will not be shown: " + why);
        forget_video();
    }
}

void Player::forget_video()
{
    // **Under the mutex, and the last reference may not be this one.** A shell
    // asking about the picture holds one for the length of its call, so what
    // this drops is the engine thread's; whoever drops the last destroys it.
    std::shared_ptr<VideoPath> letting_go;
    {
        const std::lock_guard lock{mutex_};
        letting_go = std::move(video_);
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
Player::RunEnd Player::pump(Graph& graph, Playlist& playlist)
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

        // **The track the picture belongs to, and it is the one being
        // heard.** A queue plays a playlist gaplessly inside one run and reads
        // ahead by the ring's depth, so `index()` -- what the decoder is on --
        // can be several tracks past what is coming out of the device. A
        // picture that followed it would be that far ahead of its own sound,
        // and with the ring's default depth against a short track that is the
        // whole track: measured as a picture that decoded a frame and waited
        // out the entire file before its time came.
        const auto heard = [this, &graph]() -> std::size_t {
            const std::lock_guard lock{mutex_};
            return queue_ != nullptr ? queue_->index_at(graph.position_frames()) : 0;
        };
        const auto began = [this, &graph]() -> double {
            const std::lock_guard lock{mutex_};
            if (queue_ == nullptr || source_.sample_rate == 0) {
                return 0.0;
            }
            return static_cast<double>(queue_->start_at(graph.position_frames())) /
                   static_cast<double>(source_.sample_rate);
        };
        std::size_t showing = heard();
        start_video(&audible, began());

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
            if (queue_ != nullptr && heard() != showing) {
                // **A track boundary, and the picture follows it.** The audio
                // does not stop -- that is what gapless is -- so the picture is
                // torn down and built again against the new file's feed while
                // the device keeps being fed. It costs a decoder and a
                // presenter, which is milliseconds, and it happens on this
                // thread rather than the render one.
                //
                // A file with no picture after one that had is a picture that
                // ends, and the other way round is one that begins: `open_video`
                // answers both by doing nothing when there is no video.
                showing = heard();
                stop_video();
                open_video(playlist, showing);
                start_video(&audible, began());
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

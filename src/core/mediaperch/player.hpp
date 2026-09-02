// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine, as one object: a playlist, a device, and a thread that keeps the
// two connected.
//
// **This is the product.** Everything above it -- the probe's command line, the
// CLI, a window that may never be written -- asks this the same questions
// through the same door. The probe played a file because somebody typed a path;
// the engine plays because it was told to, by whoever, and carries on when
// nobody is watching.
//
// **It is in the core because it has to be.** A playlist, a queue and the
// decision to rebuild a graph are not Windows, and putting them in the head
// would mean writing them again for the second platform. What genuinely belongs
// to the platform -- opening a file with a decoder module, opening an endpoint,
// finding a DSP stage in a DLL -- comes in through `IEngineHost`, which is the
// whole of what this needs from an operating system.
//
// **One thread owns the graph.** Commands arrive from IPC threads and are
// either applied directly, when the graph already promises that they are safe
// (pause, resume, seek), or posted, when they mean building a new graph. The
// distinction is not an optimisation: a rebuild takes milliseconds and a shell
// asking for one must not block on it.

#ifndef MEDIAPERCH_PLAYER_HPP
#define MEDIAPERCH_PLAYER_HPP

#include "mediaperch/convert.hpp"
#include "mediaperch/dsp.hpp"
#include "mediaperch/negotiation.hpp"
#include "mediaperch/passthrough.hpp"
#include "mediaperch/processed.hpp"
#include "mediaperch/protocol.hpp"
#include "mediaperch/queue.hpp"
#include "mediaperch/sink.hpp"
#include "mediaperch/source.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp {

/// What the engine needs from the operating system it happens to be on.
///
/// Four things, and no more: open a file, open a device, find a filter, say
/// something. Everything else the engine does itself.
class IEngineHost {
public:
    virtual ~IEngineHost() = default;

    /// Opens `path` with whichever decoder claims it. Returns nullptr and fills
    /// `why` when nothing does -- which is a skipped track, not a failed
    /// playlist.
    virtual std::unique_ptr<ISource> open_source(const std::string& path,
                                                 std::string& decoder,
                                                 std::string& why) = 0;

    /// Opens an endpoint. `want` is a name to match, or empty for the default;
    /// `resolved` comes back with what was actually opened, for the report.
    virtual Sink open_sink(const std::string& want, bool shared, std::string& resolved,
                           std::string& why) = 0;

    /// A DSP stage by module id, or nullptr.
    [[nodiscard]] virtual const MpDspVtbl* dsp(const std::string& id) = 0;

    /// Whether an endpoint is there at all. Asked while waiting for one that
    /// was pulled out, so it must be cheap and must not disturb anything.
    [[nodiscard]] virtual bool device_ready(const std::string& want, bool shared) = 0;

    /// One line for the log tail. Called from the engine thread.
    virtual void log(const std::string& line) = 0;
};

/// Everything a person can set, in one place.
///
/// Deliberately the same list the probe takes on its command line: two ways of
/// saying the same thing that were not the same thing would be worse than
/// either. `Player::settings()` turns this into the rows a shell shows, and
/// `Player::set()` is the only way it changes.
struct PlayerConfig {
    std::string device;          ///< a substring of an endpoint's name, or empty
    bool shared = false;
    PathPolicy path = PathPolicy::bit_exact;
    ConvertConfig conversion;
    PassthroughConfig buffering;
    /// `name` or `name:key=value,...`, in the order they run in.
    std::vector<std::string> dsp;
    bool recover = true;
    unsigned recover_timeout = 30;
};

class Player {
public:
    explicit Player(IEngineHost& host);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    /// Starts the engine thread. Nothing plays until something is asked for.
    void start();
    /// Stops everything and joins. Safe to call twice.
    void shutdown();

    // --- what is playing ----------------------------------------------------

    /// Replaces the playlist and begins at `first`.
    void play(std::vector<std::string> files, std::size_t first = 0);
    /// Adds to the end without disturbing anything.
    void enqueue(const std::vector<std::string>& files);
    void clear();

    void pause();
    void resume();
    void stop();
    /// Absolute or relative, in the current queue's frames. False when the
    /// source cannot seek or nothing is playing.
    bool seek(std::int64_t frames, bool relative);
    void next();
    void previous();

    [[nodiscard]] ipc::Status status() const;
    [[nodiscard]] std::vector<std::string> playlist() const;

    // --- settings -----------------------------------------------------------

    [[nodiscard]] std::vector<ipc::Setting> settings() const;
    /// Applies one setting. False and a reason when the value is not one.
    ///
    /// A setting that changes the path rebuilds the graph where it stands: the
    /// device stops and starts, and the audio carries on from the frame it
    /// stopped on. That is the same machinery a lost device uses, pointed at a
    /// different cause.
    bool set(const std::string& key, const std::string& value, std::string& why);

private:
    class Playlist;

    enum class RunEnd {
        finished,      ///< the playlist ended
        stopped,       ///< somebody said stop, or asked for something else
        device_lost,   ///< the endpoint went away
        format_change, ///< the next track needs the device reopened
        rebuild,       ///< a setting changed that the graph is built from
        failed,        ///< it could not start at all
    };

    struct Request {
        std::vector<std::string> files;
        std::size_t first = 0;
        std::uint64_t from = 0;
    };

    void run();
    /// One playlist, from `first`, until it ends or something interrupts it.
    void play_request(const Request& request);
    /// One device, one graph. Returns why it ended and where it was.
    RunEnd play_run(Queue& queue, Playlist& playlist, std::uint64_t& position);
    template <typename Graph>
    RunEnd pump(Graph& graph);

    /// Builds the chain from `config_.dsp`. False and a reason when a stage is
    /// not there or will not take a setting.
    bool build_chain(DspChain& chain, std::string& why);

    void set_state(ipc::State state);
    void note(const std::string& line);
    /// Waits for a device to answer again, or gives up. False also when
    /// somebody asked the engine to stop in the meantime.
    bool wait_for_device();

    IEngineHost* host_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;

    /// Set under the mutex by the engine thread while a graph exists, cleared
    /// before it is destroyed. Everything a shell asks about what is playing
    /// goes through these two, which is why they are not owned here.
    Queue* queue_ = nullptr;
    PassthroughGraph* graph_a_ = nullptr;
    ProcessedGraph* graph_b_ = nullptr;

    PlayerConfig config_;
    /// The config the current run was actually built from. A setting that turns
    /// out to be unplayable is put back to this one rather than left in place
    /// with nothing playing.
    PlayerConfig applied_;
    std::vector<std::string> files_;
    std::deque<Request> requests_;

    ipc::State state_ = ipc::State::stopped;
    /// What the last run settled on, for `status` between runs.
    std::string track_;
    std::string decoder_;
    std::string device_;
    Format source_{};
    Format wire_{};
    std::uint32_t fidelity_ = 0;
    bool processed_ = false;
    std::string error_;
    std::uint64_t position_ = 0;
    std::uint32_t index_ = 0;
    /// Across every graph this engine has built, not just the current one. A
    /// rebuild -- a lost device, a setting somebody changed -- starts a new
    /// graph with a new counter, and an underrun that stopped being counted
    /// because of that would be an underrun nobody heard about.
    std::uint64_t total_frames_ = 0;
    std::uint64_t total_underruns_ = 0;

    std::atomic<bool> quit_{false};
    std::atomic<bool> stop_wanted_{false};
    std::atomic<bool> rebuild_wanted_{false};
    bool started_ = false;
};

} // namespace mp

#endif // MEDIAPERCH_PLAYER_HPP

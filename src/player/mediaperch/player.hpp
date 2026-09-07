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
#include "mediaperch/video.hpp"
#include "mediaperch/video_path.hpp"

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
/// Seven things, and no more: open a file, open a device, find a filter, say
/// something -- and, since §9.7.1's video path moved in here, open a presenter,
/// open a video decoder, and answer when the next frame may be drawn.
/// Everything else the engine does itself.
///
/// **The two new ones are doors, not policy.** Which presenter module is
/// loaded and which decoder claims a codec is a registry's business, and a
/// registry is a `LoadLibrary` away from being portable; what to do with what
/// comes back -- hand the presenter's device to the decoder, build the graph,
/// run the loop -- is `mp::VideoPath`, and that is here.
class IEngineHost {
public:
    virtual ~IEngineHost() = default;

    /// Opens `path` with whichever demuxer claims it and whichever codec
    /// claims its audio. Returns nullptr and fills `why` when nothing does --
    /// which is a skipped track, not a failed playlist.
    ///
    /// **It opens the file, not the audio.** §4 gives a file one position, so
    /// a host that returned a bare `ISource` and left the video to be opened
    /// separately would be returning one of two demuxers -- and a seek would
    /// then have to move both and land them on the same moment. `IMedia` is
    /// the file, and what it hands out shares that position.
    virtual std::unique_ptr<IMedia> open_media(const std::string& path,
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

    /// Opens a presenter.
    ///
    /// `window` is the head's own and is opaque here -- an HWND on Windows.
    /// **Null is the engine's case and not a degraded one**: §9.7.1 has a
    /// windowless engine draw into a composition surface a shell composites,
    /// and only a tool that owns a window passes one. `module` comes back with
    /// which one it was, for the report.
    virtual std::unique_ptr<Presenter> open_presenter(void* window, std::string& module,
                                                      std::string& why) = 0;

    /// When the next frame may be drawn, for a presenter that has one.
    ///
    /// **Asked after `configure`, and that is why it is not part of opening
    /// one.** A composition presenter has no swap chain until it has been given
    /// a picture, and the event the compositor sets is made with the chain;
    /// a caller that wanted the clock at `open` would be asking before there
    /// was one. A window presenter could answer earlier and does not, because
    /// two ways of getting a clock is the drift this door exists to avoid.
    ///
    /// Null is a real answer: an off-screen presenter has nothing to pace on,
    /// and the caller supplies its own clock or does not draw. Nothing here can
    /// be built in the core -- a vertical blank and a waitable handle are both
    /// an operating system's, which is what puts this behind this interface at
    /// all.
    [[nodiscard]] virtual std::unique_ptr<IFrameClock> frame_clock(Presenter& presenter,
                                                                   void* window) = 0;

    /// A decoder for `codec`: best first, and the next one when the best
    /// declines -- the rule `open_source` follows, and for a case that was
    /// measured rather than imagined (see §9.8.1's note on codec_mft).
    ///
    /// `device` is the presenter's, or null for a decoder that works in system
    /// memory. §9.8.1 is why it is passed rather than made.
    virtual std::unique_ptr<VideoDecoder> open_video_decoder(
        MpCodec codec, const MpGraphicsDevice* device, const std::uint8_t* config,
        std::uint32_t config_bytes, std::string& module, std::string& why) = 0;

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
    /// What was typed for the shaper, kept beside what it resolved to.
    ///
    /// `shibata` and `shibata:5` are different filters and both describe
    /// themselves as "shibata: <curve>", so the resolved form cannot be fed
    /// back in. A settings file this program writes has to be one it can read,
    /// so the words are kept as well as the filter.
    std::string shaping_spec = "0";
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

    /// Opens the picture, if the track has one and a presenter will take it.
    /// **Never fatal**: a file whose video will not open is a file that plays,
    /// which is what it would have done before there was a video path at all.
    void open_video(Playlist& playlist, std::size_t index);
    /// Starts it against the clock the audio graph is now running on (§8).
    /// Non-template so that `pump` can call it -- `GraphClock<Graph>` is an
    /// `IAudioClockSource` and that is all this needs to know.
    void start_video(IAudioClockSource& audio);
    void stop_video() noexcept;

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

    /// The picture, when the current track has one. Null for most files, which
    /// is not an error and is why nothing above tests it before playing.
    ///
    /// **Owned by the engine thread**, built in `play_run` and destroyed there,
    /// so nothing takes the mutex to reach it. A rebuild -- a lost device, a
    /// changed setting -- opens it again, which costs a decoder and a presenter
    /// and is the price of the audio graph being the thing a rebuild is about.
    std::unique_ptr<VideoPath> video_;

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

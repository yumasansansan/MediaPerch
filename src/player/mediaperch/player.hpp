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
#include "mediaperch/buffering.hpp"
#include "mediaperch/calibrate.hpp"
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
/// Eight things, and no more: open a file, open a device, find a filter, say
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

    /// The same for a video stage (§9.8.3). **A separate door because it is a
    /// separate vtable**, not because finding a module is a different problem:
    /// one that answered `void*` would be a door that had given up on saying
    /// what it returns.
    [[nodiscard]] virtual const MpVideoDspVtbl* video_dsp(const std::string&)
    {
        return nullptr;
    }

    /// **Every module that is loaded**, for §10's palette: which kind, which
    /// id, what priority it declared, and whether §11's allow-list names it.
    ///
    /// A door because a registry is a `LoadLibrary` away from being portable,
    /// and no more than that: what a shell does with the list -- draws it,
    /// reorders it, filters it -- is not this interface's business. A host with
    /// no registry answers with nothing, which is what a test does.
    [[nodiscard]] virtual std::vector<ipc::ModuleRow> modules() { return {}; }

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
    /// **Whether a person chose `ring_periods`, as against it being the
    /// default.** §9.8.2's three answers are in an order that respects who said
    /// what: a number somebody typed wins outright, then a measurement made on
    /// this machine, then the engine's generous default. Without this flag the
    /// first two are indistinguishable, and a profile would quietly overrule a
    /// person -- which is the one direction this must not be wrong in.
    bool ring_periods_chosen = false;
    /// What this machine measured for itself (§9.8.2), or empty.
    ///
    /// **Handed in rather than read here.** §11: the head opens files and the
    /// portable half decides what the text means, so a head reads the file and
    /// `mp::parse_profile` turns it into this.
    Profile profile;
    /// `name` or `name:key=value,...`, in the order they run in.
    std::vector<std::string> dsp;
    /// The same for the picture (§9.8.3), which runs in linear light inside the
    /// presenter. **A separate list because it is a separate chain**: they are
    /// not alternatives and a stage cannot move between them -- one takes an
    /// f64 bus of samples and the other takes a texture.
    std::vector<std::string> video_dsp;
    bool recover = true;
    unsigned recover_timeout = 30;
};

class Player final : public ICalibrationHost {
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

    // --- the shape, for a shell that draws it (§10) --------------------------

    /// **What the engine is, as nodes and edges.**
    ///
    /// Derived every time it is asked for, from the configuration and from what
    /// is playing -- never from a model kept beside the graph, because two
    /// models of one graph are two things to keep in step and the one that
    /// drifts is the one nobody plays through.
    ///
    /// The shape depends on the path: Path A is source to sink with a repack
    /// nobody can insert into, and Path B has the converter and whatever
    /// stages a person named. A run that is not playing answers with the shape
    /// the *settings* would build, because a canvas has to be drawable before
    /// anything is playing.
    [[nodiscard]] ipc::Graph graph() const;

    /// One node's own settings, by the id `graph` gave it. Empty for a node
    /// that has none -- a sink or a source is a fact rather than a stage.
    [[nodiscard]] std::vector<ipc::Setting> node_settings(const std::string& node) const;

    /// Changes one. False and a reason for a node that has no such key.
    ///
    /// **A stage's setting is a rebuild**, exactly as `set("dsp", ...)` is: the
    /// chain is built when a graph is, and a key changed underneath a render
    /// thread that is inside `process` is a data race rather than a setting.
    /// The device stops and starts and the audio carries on from the frame it
    /// stopped on, which is the machinery a lost device already uses.
    bool set_node(const std::string& node, const std::string& key,
                  const std::string& value, std::string& why);

    /// Every module that could be a node, from the host.
    [[nodiscard]] std::vector<ipc::ModuleRow> modules() const;

    /// **The composition surface the picture is being drawn into** (§9.7.1), or
    /// zero when there is none.
    ///
    /// A number, because that is what a `HANDLE` is once it has to cross a
    /// process boundary. Making it valid in another process is the head's --
    /// duplicating one is not something the core can do -- so this hands over
    /// the value and says nothing about who may have it.
    [[nodiscard]] std::uint64_t surface() const;
    /// The buffering profile to consult (§9.8.2). Takes effect on the next
    /// track, because the ring is decided when a graph is built.
    void use_profile(Profile profile);

    // --- measuring this machine (§9.8.2) -------------------------------------

    /// **Measures what this machine needs to play these files, and keeps it.**
    ///
    /// A calibration is playback of a list at one ring size after another, so
    /// it cannot run faster than the material and it needs the device. **It
    /// takes the engine over**: what was playing stops, the sweep runs on the
    /// engine thread, and nothing else plays until it is done. The alternative
    /// -- measuring beside a playlist -- would be measuring a machine that is
    /// doing something else, which is the one thing a measurement must not do.
    ///
    /// Posted rather than run here, because a shell asking for one must not
    /// block for the afternoon it takes. Progress arrives as log lines, which
    /// a subscribed shell already shows, and `status` says what is playing --
    /// because during a calibration something is.
    void calibrate(std::vector<std::string> files, CalibrationPlan plan);

    /// The profile as text, in the form §11's file takes. Empty until a
    /// calibration has run in this process or one was handed in.
    [[nodiscard]] std::string profile_text() const;

    /// **Which display the picture is on** (§9.4, §9.7.1), from the shell that
    /// has the window. Applied at once to whatever is showing and remembered
    /// for the next track.
    ///
    /// `known` false is *work it out yourself*: the presenter probes, which is
    /// right for a window this process owns and is the only honest answer when
    /// a shell has stopped knowing.
    ///
    /// False and a reason only when a presenter refused it. **A run with no
    /// picture takes it and says nothing**, because a shell should not have to
    /// know whether the current track has video to tell the engine where its
    /// window is.
    bool set_display(bool known, const VideoPath::DisplayIs& display, std::string& why);

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

    /// A calibration nobody has started yet. Queued rather than run where it
    /// was asked for, because a shell asking for one must not block for the
    /// afternoon it takes.
    struct Sweeping {
        std::vector<std::string> files;
        CalibrationPlan plan;
    };

    void run();
    /// One calibration, on the engine thread. Takes the machine over.
    void run_sweep(const Sweeping& sweep);
    /// One playlist, from `first`, until it ends or something interrupts it.
    void play_request(const Request& request);
    /// One device, one graph. Returns why it ended and where it was.
    RunEnd play_run(Queue& queue, Playlist& playlist, std::uint64_t& position);
    template <typename Graph>
    RunEnd pump(Graph& graph, Playlist& playlist);

    /// Builds the chain from `config_.dsp`. False and a reason when a stage is
    /// not there or will not take a setting.
    bool build_chain(DspChain& chain, std::string& why);

    /// Opens the picture, if the track has one and a presenter will take it.
    /// **Never fatal**: a file whose video will not open is a file that plays,
    /// which is what it would have done before there was a video path at all.
    void open_video(Playlist& playlist, std::size_t index);
    /// The same, for a file that is not in a playlist -- which is what a
    /// calibration run is.
    void open_video(IMedia* media, std::uint32_t decoder_threads);
    /// Starts it against the clock the audio graph is now running on (§8).
    /// Non-template so that `pump` can call it -- `GraphClock<Graph>` is an
    /// `IAudioClockSource` and that is all this needs to know.
    void start_video(IAudioClockSource& audio);
    void stop_video() noexcept;

    // `ICalibrationHost`. Called from the engine thread, by `mp::calibrate`.
    bool inspect(const std::string& file, StreamShape& shape, double& duration,
                 std::string& why) override;
    bool play(const CalibrationRun& run, RunResult& result, std::string& why) override;
    void say(const std::string& line) override;

    /// One calibration run, played to the end of its window. The half of `play`
    /// that is the same whichever graph the path chose, so that the two
    /// branches are a type and not a second copy of the measurement.
    template <typename Graph>
    void measure(Graph& graph, const CalibrationRun& run, RunResult& result);

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
    /// What the shell last said the display is, applied to every picture this
    /// engine opens until it says otherwise. Under the mutex: it arrives on an
    /// IPC thread and is read by the engine thread.
    bool display_known_ = false;
    VideoPath::DisplayIs display_{};

    PlayerConfig config_;
    /// The config the current run was actually built from. A setting that turns
    /// out to be unplayable is put back to this one rather than left in place
    /// with nothing playing.
    PlayerConfig applied_;
    std::vector<std::string> files_;
    std::deque<Request> requests_;
    /// At most one at a time: a second while the first is running would be two
    /// measurements of one machine at once, which is neither of them.
    std::deque<Sweeping> sweeps_;

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

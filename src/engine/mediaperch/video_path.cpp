// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/video_path.hpp"

#include "mediaperch/video_host.hpp"
#include "mediaperch/result.hpp"

#include <cstdlib>
#include <cstring>

#include <cstdio>

namespace mp {

namespace {

/// `name` or `name:key=value,...`, with the module prefix filled in. The audio
/// chain's grammar, read the same way -- `Player::build_chain` is the other
/// reader and they are deliberately the same rules.
std::pair<std::string, std::vector<std::pair<std::string, std::string>>> split_stage(
    const std::string& spec)
{
    std::pair<std::string, std::vector<std::pair<std::string, std::string>>> out;
    const std::size_t colon = spec.find(':');
    out.first = spec.substr(0, colon);
    if (out.first.rfind("vdsp_", 0) != 0) {
        out.first = "vdsp_" + out.first;
    }
    if (colon == std::string::npos) {
        return out;
    }
    std::string rest = spec.substr(colon + 1);
    std::size_t at = 0;
    while (at <= rest.size()) {
        const std::size_t comma = rest.find(',', at);
        const std::string one =
            rest.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const std::size_t equals = one.find('=');
        if (equals != std::string::npos) {
            out.second.emplace_back(one.substr(0, equals), one.substr(equals + 1));
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }
    return out;
}

} // namespace

const std::string& VideoPath::stage_module(std::size_t index) const noexcept
{
    static const std::string none;
    return index < stage_modules_.size() ? stage_modules_[index] : none;
}

void VideoPath::open_stages(IVideoHost& host, const Config& want)
{
    stages_.clear();
    stage_modules_.clear();
    if (want.stages.empty() || presenter_ == nullptr) {
        return;
    }

    MpGraphicsDevice device{};
    device.size = sizeof(device);
    const bool have_device = presenter_->get_device(device) == MP_OK;

    for (const std::string& spec : want.stages) {
        const auto [id, settings] = split_stage(spec);
        const MpVideoDspVtbl* vtbl = host.video_dsp(id);
        if (vtbl == nullptr) {
            continue;
        }
        auto stage = std::make_unique<VideoStage>();
        if (stage->open(*vtbl, have_device ? &device : nullptr) != MP_OK) {
            continue;
        }
        bool refused = false;
        for (const auto& [key, value] : settings) {
            if (stage->set(key.c_str(), value.c_str()) != MP_OK) {
                refused = true;
                break;
            }
        }
        if (refused) {
            continue;
        }
        stages_.push_back(std::move(stage));
        stage_modules_.push_back(id);
    }
}

bool VideoPath::hand_over(std::string& why)
{
    if (presenter_ == nullptr) {
        why = "there is no presenter to give a chain to";
        return false;
    }
    std::vector<MpVideoStage> handed;
    handed.reserve(stages_.size());
    for (const std::unique_ptr<VideoStage>& stage : stages_) {
        handed.push_back(stage->handed());
    }
    const MpResult told = presenter_->stages(
        handed.empty() ? nullptr : handed.data(), static_cast<std::uint32_t>(handed.size()));
    if (told == MP_ERR_UNSUPPORTED && handed.empty()) {
        // A presenter with no chain asked for nothing is a presenter that is
        // already doing what was wanted.
        return true;
    }
    if (told != MP_OK) {
        why = std::string{"the presenter would not take a chain: "} + result_name(told);
        return false;
    }
    return true;
}

std::vector<std::string> VideoPath::stage_describe(std::size_t index,
                                                   std::chrono::milliseconds deadline)
{
    const std::lock_guard lock{gate_};
    std::vector<std::string> out;
    if (index >= stages_.size()) {
        return out;
    }
    const auto ask = [&] {
        char line[512];
        for (std::uint32_t row = 0;; ++row) {
            line[0] = '\0';
            if (stages_[index]->describe(row, line, sizeof line) != MP_OK) {
                break;
            }
            out.emplace_back(line);
        }
    };
    if (!running()) {
        ask();
        return out;
    }
    loop_->hold();
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ask();
    loop_->release();
    return out;
}

bool VideoPath::set_stage(std::size_t index, const std::string& key,
                          const std::string& value, std::string& why,
                          std::chrono::milliseconds deadline)
{
    const std::lock_guard lock{gate_};
    if (index >= stages_.size()) {
        why = "there is no such stage in the chain";
        return false;
    }
    const auto say = [&] {
        const MpResult told = stages_[index]->set(key.c_str(), value.c_str());
        if (told != MP_OK) {
            why = stage_modules_[index] + " would not take `" + key + " = " + value +
                  "`: " + result_name(told);
            return false;
        }
        return true;
    };
    if (!running()) {
        return say();
    }
    loop_->hold();
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool ok = say();
    loop_->release();
    return ok;
}

std::uint64_t VideoPath::surface() noexcept
{
    const std::lock_guard lock{gate_};
    if (presenter_ == nullptr) {
        return 0;
    }
    char line[256];
    for (std::uint32_t row = 0;; ++row) {
        line[0] = '\0';
        if (presenter_->describe(row, line, sizeof line) != MP_OK) {
            break;
        }
        if (std::strncmp(line, "surface\t", 8) != 0) {
            continue;
        }
        const char* at = std::strstr(line + 8, "composition 0x");
        if (at == nullptr) {
            return 0; // a window, or off-screen: there is nothing to hand over
        }
        return std::strtoull(at + 14, nullptr, 16);
    }
    return 0;
}

std::vector<std::string> VideoPath::presenter_describe()
{
    const std::lock_guard lock{gate_};
    std::vector<std::string> out;
    if (presenter_ == nullptr) {
        return out;
    }
    char line[256];
    for (std::uint32_t row = 0;; ++row) {
        line[0] = '\0';
        if (presenter_->describe(row, line, sizeof line) != MP_OK) {
            break;
        }
        out.emplace_back(line);
    }
    return out;
}

VideoGraph::Stats VideoPath::graph_stats() const
{
    const std::lock_guard lock{gate_};
    return graph_ != nullptr ? graph_->stats() : VideoGraph::Stats{};
}

DisplayLoop::Stats VideoPath::loop_stats() const
{
    const std::lock_guard lock{gate_};
    return thread_.joinable() && loop_ != nullptr ? loop_->stats() : DisplayLoop::Stats{};
}

VideoPath::~VideoPath()
{
    stop();
}

bool VideoPath::open(IVideoHost& host, void* window, IPacketFeed& feed,
                     const MpVideoInfo& picture, MpCodec codec,
                     const std::uint8_t* config, std::uint32_t config_bytes,
                     const Config& want, std::string& why)
{
    const std::lock_guard lock{gate_};
    stop();
    graph_.reset();
    own_clock_.reset();
    own_frames_.reset();
    decoder_.reset();
    // **Before the presenter**, which is holding them: a stage closed after the
    // thing that was handed it would be a stage the presenter could still run.
    stages_.clear();
    stage_modules_.clear();
    presenter_.reset();
    modules_ = Modules{};

    // **The presenter first, because the decoder wants its device** (§9.8.1).
    // A decoder that makes its own D3D11 device produces textures this
    // presenter cannot sample without a copy through system memory, which is
    // the whole of why the order is this way round and not the other.
    std::unique_ptr<Presenter> presenter = host.open_presenter(window, modules_.presenter, why);
    if (presenter == nullptr) {
        return false;
    }

    // Before `configure`, which is where the target is made.
    if (want.width != 0 || want.height != 0) {
        char value[32];
        std::snprintf(value, sizeof value, "%ux%u", want.width, want.height);
        const MpResult told = presenter->set("size", value);
        if (told != MP_OK) {
            why = std::string{"the presenter would not render at "} + value + ": " +
                  result_name(told);
            return false;
        }
    }
    if (presenter->configure(picture) != MP_OK) {
        why = "the presenter would not take that picture";
        return false;
    }

    MpGraphicsDevice device{};
    const bool have_device = presenter->get_device(device) == MP_OK;

    std::unique_ptr<VideoDecoder> decoder = host.open_video_decoder(
        codec, have_device ? &device : nullptr, config, config_bytes, modules_.decoder, why);
    if (decoder == nullptr) {
        return false;
    }
    if (want.decoder_threads != 0) {
        // **Between `open` and the first packet**, which is where a decoder
        // that starts a worker pool can still be told how big to make it.
        const std::string count = std::to_string(want.decoder_threads);
        const MpResult told = decoder->set("threads", count.c_str());
        if (told != MP_OK) {
            why = modules_.decoder + " would not take " + count + " threads: " +
                  result_name(told);
            return false;
        }
    }

    presenter_ = std::move(presenter);
    // **After `configure`, because a stage opens on the presenter's device**
    // (§9.8.1) and there is none until then; and before the first frame, so
    // nothing is shown ungraded that was meant to be graded.
    open_stages(host, want);
    if (std::string trouble; !hand_over(trouble)) {
        // Not fatal: a presenter with no chain is a presenter, and a picture
        // that is not graded is better than no picture (§9.1's argument).
        why = trouble;
        stages_.clear();
        stage_modules_.clear();
    }
    decoder_ = std::move(decoder);
    graph_ = std::make_unique<VideoGraph>(feed, *decoder_, *presenter_, picture);
    // **After `configure`, which is where the chain the event belongs to is
    // made.** Null is a real answer and not a failure: an off-screen presenter
    // has nothing that will show what it draws, so nothing that says when.
    own_frames_ = host.frame_clock(*presenter_, window);
    window_ = window;
    return true;
}

bool VideoPath::retrack(IVideoHost& host, IPacketFeed& feed, const MpVideoInfo& picture,
                        MpCodec codec, const std::uint8_t* config,
                        std::uint32_t config_bytes, const Config& want, std::string& why)
{
    const std::lock_guard lock{gate_};
    if (presenter_ == nullptr) {
        why = "there is no presenter to put the next track on";
        return false;
    }
    stop();
    // **The graph before the decoder**, because it holds one.
    graph_.reset();
    decoder_.reset();
    own_frames_.reset();

    // The presenter keeps what it was told -- the size a shell asked for, the
    // display it named, the chain -- and is told what the new file is.
    if (presenter_->configure(picture) != MP_OK) {
        why = "the presenter would not take that picture";
        return false;
    }

    MpGraphicsDevice device{};
    const bool have_device = presenter_->get_device(device) == MP_OK;
    std::unique_ptr<VideoDecoder> decoder = host.open_video_decoder(
        codec, have_device ? &device : nullptr, config, config_bytes, modules_.decoder, why);
    if (decoder == nullptr) {
        return false;
    }
    if (want.decoder_threads != 0) {
        const std::string count = std::to_string(want.decoder_threads);
        const MpResult told = decoder->set("threads", count.c_str());
        if (told != MP_OK) {
            why = modules_.decoder + " would not take " + count + " threads: " +
                  result_name(told);
            return false;
        }
    }
    decoder_ = std::move(decoder);
    graph_ = std::make_unique<VideoGraph>(feed, *decoder_, *presenter_, picture);
    // **Asked for again, because `configure` may have replaced the chain** and
    // the waitable object belongs to the chain rather than to the presenter.
    own_clock_.reset();
    own_frames_ = host.frame_clock(*presenter_, window_);
    return true;
}

bool VideoPath::start(IMediaClock* follow, std::string& why, double origin_seconds)
{
    if (own_frames_ == nullptr) {
        why = "this presenter has no clock to pace on";
        return false;
    }
    return start(follow, *own_frames_, why, origin_seconds);
}

bool VideoPath::start(IMediaClock* follow, IFrameClock& frames, std::string& why,
                      double origin_seconds)
{
    const std::lock_guard lock{gate_};
    if (graph_ == nullptr) {
        why = "there is no video graph to run";
        return false;
    }
    if (running()) {
        why = "the display loop is already turning";
        return false;
    }
    ended_.store(false, std::memory_order_release);
    frames_ = &frames;
    own_clock_.reset();
    if (follow == nullptr) {
        // **The video engine's own timeline.** Counted on the frame clock's
        // counter, because that is what the loop stamps its turns with, and a
        // reading stamped from another counter would be a reading from another
        // time. Started after the loop is built and before it turns, so the
        // first frame is not late by the time the setup took.
        own_clock_ = std::make_unique<FreeClock>(frames, k_own_rate);
        follow = own_clock_.get();
    }
    loop_ = std::make_unique<DisplayLoop>(*graph_, *follow, frames, origin_seconds);
    if (own_clock_ != nullptr) {
        own_clock_->start();
    }
    thread_ = std::thread{[this] {
        loop_->run();
        ended_.store(true, std::memory_order_release);
    }};
    return true;
}

bool VideoPath::seek_alone(double seconds, const std::function<bool(double)>& move,
                           std::string& why, std::chrono::milliseconds deadline)
{
    const std::lock_guard lock{gate_};
    if (own_clock_ == nullptr || loop_ == nullptr || graph_ == nullptr) {
        why = "the picture is not running on its own clock";
        return false;
    }
    loop_->hold();
    // The same wait `seek_together` takes, for the same reason: a turn taken
    // while the file moves is a turn deciding about a frame from a place
    // nobody is at any more.
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && !ended() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool moved = move(seconds);
    // Even when it did not move: see `seek_together`.
    graph_->rewound();
    own_clock_->seek(static_cast<std::uint64_t>(seconds * k_own_rate));
    loop_->release();
    if (!moved) {
        why = "the file would not move";
    }
    return moved;
}

void VideoPath::stop() noexcept
{
    const std::lock_guard lock{gate_};
    if (!thread_.joinable()) {
        frames_ = nullptr;
        return;
    }
    // **The clock, not the loop.** `run` returns when a turn says stop, and a
    // turn is inside `wait` for a whole refresh; telling the clock is what
    // makes that return now rather than in sixteen milliseconds. A hold would
    // not do it -- a held loop keeps turning, which is the point of a hold.
    if (frames_ != nullptr) {
        frames_->cancel();
    }
    thread_.join();
    frames_ = nullptr;
}

bool VideoPath::tell(const char* key, const char* value, std::string& why,
                     std::chrono::milliseconds deadline)
{
    const std::lock_guard lock{gate_};
    if (presenter_ == nullptr) {
        why = "there is no presenter to tell";
        return false;
    }
    const auto say = [&] {
        const MpResult told = presenter_->set(key, value);
        if (told != MP_OK) {
            why = std::string{"the presenter would not take "} + key + " = " + value + ": " +
                  result_name(told);
            return false;
        }
        return true;
    };
    if (!running()) {
        return say();
    }

    loop_->hold();
    // The same wait `seek_together` takes, and for the same reason it takes a
    // deadline: a loop whose display has gone away has no turn in which to
    // answer, and refusing the message because nobody is drawing would be
    // refusing for the wrong reason.
    const auto give_up_at = std::chrono::steady_clock::now() + deadline;
    while (!loop_->parked() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool ok = say();
    loop_->release();
    return ok;
}

bool VideoPath::set_size(std::uint32_t width, std::uint32_t height, std::string& why,
                         std::chrono::milliseconds deadline)
{
    char value[32];
    if (width == 0 || height == 0) {
        std::snprintf(value, sizeof value, "native");
    } else {
        std::snprintf(value, sizeof value, "%ux%u", width, height);
    }
    return tell("size", value, why, deadline);
}

bool VideoPath::set_display(const DisplayIs& display, std::string& why,
                            std::chrono::milliseconds deadline)
{
    // Every field, every time. A message that carried only what changed would
    // put the presenter's idea of the display and the shell's out of step the
    // first time one was dropped, and there is nothing to gain: this is sent
    // when a window crosses a monitor, which is rare.
    char value[128];
    std::snprintf(value, sizeof value, "hdr=%d,wide=%d,white=%.4f,peak=%.4f",
                  display.hdr ? 1 : 0, display.wide ? 1 : 0,
                  static_cast<double>(display.white_nits),
                  static_cast<double>(display.peak_nits));
    return tell("display", value, why, deadline);
}

bool VideoPath::probe_display(std::string& why, std::chrono::milliseconds deadline)
{
    return tell("display", "probe", why, deadline);
}

} // namespace mp

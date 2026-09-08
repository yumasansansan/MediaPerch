// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/engine_host.hpp"

#include "mediaperch/display_win.hpp"
#include "mediaperch/packet.hpp"
#include "mediaperch/result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace mp::win {
namespace {

/// The compositor's event, read back off the settings surface.
///
/// **A number, because the ABI says a number.** The handle has to cross a
/// process boundary and a shell duplicates it out of an IPC message, so
/// `describe` prints it; reading it back here is that same conversation with
/// one fewer process in it. Null when there is none -- a window presenter, or
/// one that has not been configured and so has no swap chain yet.
void* waitable_of(Presenter& presenter)
{
    char line[256];
    for (std::uint32_t row = 0;; ++row) {
        line[0] = '\0';
        if (presenter.describe(row, line, sizeof line) != MP_OK) {
            break;
        }
        if (std::strncmp(line, "surface\t", 8) != 0) {
            continue;
        }
        const char* at = std::strstr(line + 8, "waitable 0x");
        if (at == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(std::strtoull(at + 11, nullptr, 16)));
    }
    return nullptr;
}

std::string lowered(std::string_view s)
{
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}


/// A demuxer that hands over frames rather than packets, so there is nothing
/// to route and no second stream to route it to.
///
/// `demux_mf` and `demux_ffmpeg` are these: they decode for themselves, which
/// the ABI marks with MP_STREAM_SELF_DECODES and `PacketSource` refuses to read
/// through a router. A file they claim has audio and nothing this tree can
/// present, so it is a file with no picture -- which is the ordinary case
/// anyway, and is why this exists rather than being an error.
class PlainMedia final : public IMedia {
public:
    bool open(const MpDemuxVtbl& vtbl, const std::string& path,
              const PacketSource::FindCodec& find, std::string& why)
    {
        return audio_.open(vtbl, path.c_str(), find, why);
    }

    [[nodiscard]] ISource* audio() noexcept override { return &audio_; }
    [[nodiscard]] const std::string& decoder() const noexcept override { return by_; }
    void opened_by(std::string id) { by_ = std::move(id); }

private:
    PacketSource audio_;
    std::string by_;
};

/// One file, one demuxer, one position (§4).
///
/// **The router is there even when the file has no video**, with one stream in
/// it. A second code path for the common case would be a second place for a
/// seek to behave differently, and it costs nothing measurable: a consumer
/// asking for its own stream has the demuxer read straight into that
/// consumer's buffer, and the queue it would otherwise wait in is never
/// touched.
class RoutedMedia final : public IMedia {
public:
    bool open(const MpDemuxVtbl& vtbl, const std::string& path,
              const PacketSource::FindCodec& find, std::string& why);

    [[nodiscard]] ISource* audio() noexcept override { return have_audio_ ? &audio_ : nullptr; }
    [[nodiscard]] IPacketFeed* video() noexcept override { return video_; }
    [[nodiscard]] Picture picture() const noexcept override { return picture_; }
    [[nodiscard]] const std::string& decoder() const noexcept override { return by_; }
    void opened_by(std::string id) { by_ = std::move(id); }

    bool seek_picture(double seconds) override
    {
        if (!router_ || video_ == nullptr || picture_.info.timescale == 0 || seconds < 0.0) {
            return false;
        }
        // In the picture's own ticks, which is what §9.9 says a video seek is
        // counted in; the router moves the one position and empties its queues.
        const auto ticks = static_cast<std::uint64_t>(seconds * picture_.info.timescale);
        return router_->seek(video_stream_, ticks) == MP_OK;
    }

    /// Whether the audio stream decodes itself, which is the one shape this
    /// class cannot serve. Asked of an already-open demuxer, before anything
    /// else is.
    [[nodiscard]] static bool self_decoding_audio(Demux& demux);

private:
    Demux demux_;
    /// `optional` rather than a member because a router is neither copyable nor
    /// movable and is built from a demuxer that has already had its streams
    /// selected -- two things that cannot happen in a member initialiser list.
    std::optional<PacketRouter> router_;
    PacketSource audio_;
    bool have_audio_ = false;
    IPacketFeed* video_ = nullptr;
    std::uint32_t video_stream_ = 0;
    Picture picture_{};
    std::vector<std::uint8_t> config_;
    std::string by_;
};

bool RoutedMedia::self_decoding_audio(Demux& demux)
{
    for (std::uint32_t i = 0; i < demux.stream_count(); ++i) {
        MpStreamInfo info{};
        if (!demux.stream_info(i, info) || info.kind != MP_STREAM_AUDIO) {
            continue;
        }
        return (info.flags & MP_STREAM_SELF_DECODES) != 0;
    }
    return false;
}

bool RoutedMedia::open(const MpDemuxVtbl& vtbl, const std::string& path,
                       const PacketSource::FindCodec& find, std::string& why)
{
    if (demux_.open(vtbl, path.c_str()) != MP_OK) {
        why = "the container would not open";
        return false;
    }

    std::uint32_t audio_stream = 0;
    std::uint32_t video_stream = 0;
    bool have_audio = false;
    bool have_video = false;
    MpStreamInfo video_info{};
    for (std::uint32_t i = 0; i < demux_.stream_count(); ++i) {
        MpStreamInfo info{};
        if (!demux_.stream_info(i, info)) {
            continue;
        }
        if (info.kind == MP_STREAM_AUDIO && !have_audio) {
            audio_stream = i;
            have_audio = true;
        }
        if (info.kind == MP_STREAM_VIDEO && !have_video) {
            video_stream = i;
            video_info = info;
            have_video = true;
        }
    }
    if (!have_audio && !have_video) {
        why = "nothing in it to play: no audio track and no picture";
        return false;
    }

    // **A file with no audio is a file that plays.** Its picture goes up on
    // the video engine's own clock, which is what `VideoPath::start` does with
    // nothing to follow; the router still owns the one position (§4), with one
    // consumer of it instead of two.
    std::vector<std::uint32_t> selected;
    if (have_audio) {
        selected.push_back(audio_stream);
    }
    if (have_video) {
        selected.push_back(video_stream);
    }
    if (demux_.select_streams(selected) != MP_OK) {
        // **A container that will not serve both is still a file with audio in
        // it.** What is dropped is the picture, not the track: refusing the
        // whole file because its video could not be read alongside would be a
        // player that plays less than the one before it did.
        if (!have_audio) {
            why = "the container would not serve its video stream";
            return false;
        }
        if (!have_video) {
            why = "the container would not serve its audio stream";
            return false;
        }
        have_video = false;
        selected.assign(1, audio_stream);
        if (demux_.select_streams(selected) != MP_OK) {
            why = "the container would not serve its audio stream";
            return false;
        }
    }

    router_.emplace(demux_, selected);
    if (have_audio) {
        if (!audio_.open(*router_, audio_stream, find, why)) {
            return false;
        }
        have_audio_ = true;
    }

    if (have_video) {
        MpVideoInfo info{};
        info.size = sizeof(info);
        if (demux_.video_info(video_stream, info) && info.width != 0) {
            (void)demux_.stream_config(video_stream, config_);
            picture_.info = info;
            picture_.codec = video_info.codec;
            picture_.config = config_.empty() ? nullptr : config_.data();
            picture_.config_bytes = static_cast<std::uint32_t>(config_.size());
            picture_.duration_ms = video_info.duration_ms;
            video_stream_ = video_stream;
            video_ = router_->feed(video_stream);
        }
        // A container that will not describe its video stream leaves `video()`
        // null, and nothing above has to know why: it is a file with audio in
        // it, which is what most files are.
    }
    if (!have_audio_ && video_ == nullptr) {
        why = "no audio track, and the container would not describe its picture";
        return false;
    }
    return true;
}

} // namespace

std::unique_ptr<IMedia> EngineHost::open_media(const std::string& path, std::string& why)
{
    return open_media(path, {}, why);
}

std::unique_ptr<IMedia> EngineHost::open_media(const std::string& path,
                                               std::string_view prefer, std::string& why)
{
    // **The container decides, and nothing is tried.** A demuxer identifies the
    // file from its first bytes, is opened, and says what is in it; each stream
    // names its codec and the codec is looked up. There is no second list to
    // fall back to any more -- MP_KIND_DECODER and the eight modules that used
    // it are gone.
    //
    // What remains is a *ranked* list of demuxers rather than one, because
    // claiming a container and reading what is inside it are different
    // questions: `demux_mp4` refuses a QuickTime sample entry it does not
    // implement, `demux_ogg` reads an Ogg carrying Speex. Those fall through to
    // the next candidate, which is FFmpeg, and that is a container reader
    // declining a codec rather than a probe having guessed wrong.
    if (!ModuleRegistry::readable(path)) {
        why = "no such file, or it is empty";
        return nullptr;
    }

    auto ranked = registry_->demuxers_for(path, prefer);

    // A preference is a reordering, not a veto: a module somebody named that
    // does not recognise this file is not a reason to refuse the file. (An
    // explicit `prefer` argument is different and does veto -- that is one
    // caller saying "use that one", and `demuxers_for` returns it alone.)
    for (auto wanted = prefer_.rbegin(); wanted != prefer_.rend(); ++wanted) {
        const auto found = std::find_if(ranked.begin(), ranked.end(), [&](const auto& c) {
            return *wanted == c.desc->id;
        });
        if (found != ranked.end()) {
            std::rotate(ranked.begin(), found, found + 1);
        }
    }

    const auto find = [this](MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes) {
        return registry_->codec_for(codec, config, config_bytes);
    };
    // Whether `why` is already a module's verdict on the *contents*, which no
    // later module's verdict on the container may overwrite.
    bool judged = false;
    for (const auto& candidate : ranked) {
        // Which of the two shapes this file is, asked of the file rather than
        // of the module's name: a demuxer that decodes for itself hands over
        // frames, and there is nothing for a router to route.
        Demux probe;
        bool self_decodes = false;
        const bool readable = probe.open(*candidate.vtbl, path.c_str()) == MP_OK;
        if (readable) {
            self_decodes = RoutedMedia::self_decoding_audio(probe);
        }
        probe.close();

        std::string trouble;
        if (self_decodes) {
            auto media = std::make_unique<PlainMedia>();
            if (media->open(*candidate.vtbl, path, find, trouble)) {
                media->opened_by(candidate.desc->id);
                return media;
            }
        } else {
            auto media = std::make_unique<RoutedMedia>();
            if (media->open(*candidate.vtbl, path, find, trouble)) {
                media->opened_by(candidate.desc->id);
                return media;
            }
        }
        // A container that would not open and a codec nobody has are different
        // failures, and the message says which. **The module that read the
        // container is the one to believe.** `demux_mkv` opens a video-only
        // WebM and says *no audio track in it*; `demux_ffmpeg`, ranked level
        // with it and tried next, will not open a file with no audio stream at
        // all; and its *the container would not open* used to be the last
        // word, which is what the log said about three valid files. A verdict
        // from a module that read the file stands; until there is one, the
        // latest refusal is kept in case nothing does better.
        if (!judged) {
            why = trouble;
            judged = readable;
        }
    }

    if (why.empty()) {
        why = prefer.empty() ? "no module recognised it"
                             : "no module called `" + std::string{prefer} + "` is loaded";
    }
    return nullptr;
}

bool EngineHost::resolve(const std::string& want, std::string& id, std::string& name,
                         std::string& why) const
{
    const MpSinkVtbl* vtbl = registry_->sink();
    if (vtbl == nullptr) {
        why = "no sink module is loaded";
        return false;
    }
    if (want.empty()) {
        id.clear();
        name = "(default endpoint)";
        return true;
    }
    const std::string wanted = lowered(want);
    std::vector<std::pair<std::string, std::string>> hits; // name, id
    MpDeviceInfo info{};
    for (std::uint32_t index = 0;; ++index) {
        info.size = sizeof(info);
        const MpResult r = vtbl->enumerate(index, &info);
        if (r == MP_END) {
            break;
        }
        if (r != MP_OK) {
            why = std::string{"enumerate failed: "} + result_name(r);
            return false;
        }
        if (lowered(info.name).find(wanted) != std::string::npos) {
            hits.emplace_back(info.name, info.id);
        }
    }
    if (hits.empty()) {
        why = "no endpoint matches `" + want + "`";
        return false;
    }
    if (hits.size() > 1) {
        // Refused rather than guessed: "the first one that matched" is how a
        // person ends up listening to the wrong device and blaming the file.
        why = "`" + want + "` matches " + std::to_string(hits.size()) + " endpoints";
        for (const auto& hit : hits) {
            why += "\n  " + hit.first;
        }
        return false;
    }
    name = hits.front().first;
    id = hits.front().second;
    return true;
}

Sink EngineHost::open_sink(const std::string& want, bool shared, std::string& resolved,
                           std::string& why)
{
    std::string id;
    if (!resolve(want, id, resolved, why)) {
        return {};
    }
    const MpSinkVtbl* vtbl = registry_->sink();
    MpSink* handle = nullptr;
    const MpResult r = vtbl->open(id.empty() ? nullptr : id.c_str(),
                                  shared ? MP_SHARE_SHARED : MP_SHARE_EXCLUSIVE, &handle);
    if (r != MP_OK) {
        why = std::string{"could not open "} + resolved + ": " + result_name(r);
        return {};
    }
    return Sink{vtbl, handle};
}

std::unique_ptr<Presenter> EngineHost::open_presenter(void* window, std::string& module,
                                                      std::string& why)
{
    const MpModuleDesc* desc = nullptr;
    const MpVideoVtbl* vtbl = registry_->video({}, &desc);
    if (vtbl == nullptr || desc == nullptr) {
        why = "no presenter module is loaded";
        return nullptr;
    }

    auto presenter = std::make_unique<Presenter>();
    if (presenter->open(*vtbl, window) != MP_OK) {
        why = std::string{desc->id} + " would not open a presenter";
        return nullptr;
    }
    if (window == nullptr) {
        // **§9.7.1, as the setting the presenter takes.** Asked before
        // `configure`, because it decides what kind of swap chain there is,
        // and refused loudly rather than quietly falling back to a texture
        // nobody composites -- an engine whose picture goes nowhere is worse
        // than one that says it cannot draw.
        const MpResult told = presenter->set("surface", "composition");
        if (told != MP_OK) {
            why = std::string{desc->id} + " has no composition surface: " +
                  result_name(told);
            return nullptr;
        }
    }
    module = desc->id;
    return presenter;
}

std::unique_ptr<IFrameClock> EngineHost::frame_clock(Presenter& presenter, void* window)
{
    // A window paces on the vertical blank of the output it is actually on;
    // §9.4 is why that is not adapter zero's.
    if (window != nullptr) {
        return VBlankClock::open(window);
    }
    // Otherwise the compositor says, if there is a compositor. An off-screen
    // presenter answers null, which is honest: nothing is going to show what it
    // draws, so nothing is going to say when.
    void* waitable = waitable_of(presenter);
    if (waitable == nullptr) {
        return nullptr;
    }
    // **The compositor's clock, not the swap chain's waitable.** They look like
    // the same question and are not: the waitable is signalled by presenting,
    // so a loop that waits every turn and presents only when a frame is due
    // runs out of credits on the turns that drew nothing. See `CompositorClock`
    // for what that measured like. The waitable is still what says the chain is
    // ready for another frame, and that belongs in front of `Present`.
    if (std::unique_ptr<CompositorClock> composed = CompositorClock::open()) {
        return composed;
    }
    return std::make_unique<WaitableClock>(waitable);
}

std::unique_ptr<VideoDecoder> EngineHost::open_video_decoder(
    MpCodec codec, const MpGraphicsDevice* device, const std::uint8_t* config,
    std::uint32_t config_bytes, std::string& module, std::string& why)
{
    // **The API is part of the question** (§9.8.1): a decoder that can hand
    // this presenter a texture it can sample scores differently from one that
    // cannot. It comes off the device rather than being named here, so a head
    // that grows a D3D12 presenter asks the right question without this
    // function being touched.
    const auto choices = registry_->video_codecs_for(
        codec, device != nullptr ? device->api : MP_GRAPHICS_NONE, config, config_bytes);
    if (choices.empty()) {
        why = "nothing here decodes that video codec";
        return nullptr;
    }

    // Best first, and the next one when the best declines -- see
    // `video_codecs_for`, which is where the measurement that forced this is
    // written down.
    auto decoder = std::make_unique<VideoDecoder>();
    for (const auto& choice : choices) {
        if (decoder->open(*choice.vtbl, codec, device, config, config_bytes) == MP_OK) {
            module = choice.desc->id;
            return decoder;
        }
        decoder->close();
    }
    why = "none of the " + std::to_string(choices.size()) +
          " decoders for that codec would open this stream";
    return nullptr;
}

std::vector<ipc::ModuleRow> EngineHost::modules()
{
    // **Every kind, not the ones a canvas happens to draw today.** A shell that
    // does not know a kind number can still list it and say so; one that never
    // saw it could not. `MpKind` goes on the wire as its number for the reason
    // `Dimension` does -- a word is a second spelling to keep in step.
    std::vector<ipc::ModuleRow> out;
    for (const MpModuleDesc* found : registry_->all()) {
        if (found == nullptr) {
            continue;
        }
        const MpModuleDesc& desc = *found;
        ipc::ModuleRow row;
        row.kind = static_cast<std::uint32_t>(desc.kind);
        row.id = desc.id != nullptr ? desc.id : "";
        row.name = desc.name != nullptr ? desc.name : "";
        row.priority = desc.priority;
        // Loaded means admitted: §11's allow-list is applied at `scan`, so a
        // module that is here passed it. A shell editing that list is editing
        // the file, which §10 says is its own decision to make.
        row.allowed = true;
        out.push_back(std::move(row));
    }
    return out;
}

const MpDspVtbl* EngineHost::dsp(const std::string& id)
{
    return registry_->dsp(id);
}

const MpVideoDspVtbl* EngineHost::video_dsp(const std::string& id)
{
    return registry_->video_dsp(id);
}

bool EngineHost::device_ready(const std::string& want, bool shared)
{
    // Opening it is the test rather than enumerating it: an endpoint can be
    // listed while its driver is still coming back, and one that will not open
    // is not one to rebuild on.
    std::string id;
    std::string name;
    std::string why;
    if (!resolve(want, id, name, why)) {
        return false;
    }
    const MpSinkVtbl* vtbl = registry_->sink();
    MpSink* handle = nullptr;
    if (vtbl->open(id.empty() ? nullptr : id.c_str(),
                   shared ? MP_SHARE_SHARED : MP_SHARE_EXCLUSIVE, &handle) != MP_OK) {
        return false;
    }
    vtbl->close(handle);
    return true;
}

void EngineHost::log(const std::string& line)
{
    log_->add(line);
}

} // namespace mp::win

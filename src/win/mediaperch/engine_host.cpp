// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/engine_host.hpp"

#include "mediaperch/packet.hpp"
#include "mediaperch/result.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace mp::win {
namespace {

std::string lowered(std::string_view s)
{
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

std::unique_ptr<ISource> EngineHost::open_source(const std::string& path,
                                                 std::string& decoder, std::string& why)
{
    return open_source(path, {}, decoder, why);
}

std::unique_ptr<ISource> EngineHost::open_source(const std::string& path,
                                                 std::string_view prefer,
                                                 std::string& decoder, std::string& why)
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
    for (const auto& candidate : ranked) {
        auto source = std::make_unique<PacketSource>();
        std::string trouble;
        if (source->open(*candidate.vtbl, path.c_str(), find, trouble)) {
            decoder = candidate.desc->id;
            return source;
        }
        // A container that would not open and a codec nobody has are different
        // failures, and the message says which. Keep the last one in case
        // nothing else does better.
        why = trouble;
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

const MpDspVtbl* EngineHost::dsp(const std::string& id)
{
    return registry_->dsp(id);
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

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/engine_host.hpp"

#include "mediaperch/decoder.hpp"
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
    // **The container first.** A demuxer identifies the file, says what is in
    // it, and the codec is looked up by name. Nothing is tried.
    //
    // The v1 path below is what runs while the modules are still being moved
    // across, and it goes with the last of them -- see docs/plan.md §4, *ABI
    // v2: the container decides*.
    for (const auto& candidate : registry_->demuxers_for(path, {})) {
        auto source = std::make_unique<PacketSource>();
        std::string trouble;
        const auto find = [this](MpCodec codec) {
            return registry_->codec_for(codec, nullptr, 0);
        };
        if (source->open(*candidate.vtbl, path.c_str(), find, trouble)) {
            decoder = candidate.desc->id;
            return source;
        }
        // A container that opened and a codec nobody has are different
        // failures, and the message says which. Keep it in case nothing else
        // does better.
        why = trouble;
    }

    auto ranked = registry_->decoders_for(path, {});
    if (ranked.empty()) {
        why = "no decoder recognised it";
        return nullptr;
    }
    // A preference is a reordering, not a veto: a decoder somebody named that
    // does not recognise this file is not a reason to refuse the file.
    for (auto wanted = prefer_.rbegin(); wanted != prefer_.rend(); ++wanted) {
        const auto found = std::find_if(ranked.begin(), ranked.end(), [&](const auto& c) {
            return *wanted == c.desc->id;
        });
        if (found != ranked.end()) {
            std::rotate(ranked.begin(), found, found + 1);
        }
    }
    // Highest score first, but a decoder may still refuse what it recognised --
    // decode_mf declines multichannel ALAC, decode_native decodes a 32-bit FLAC
    // to nothing -- so the answer is the first that actually opens.
    for (const auto& candidate : ranked) {
        auto opened = std::make_unique<Decoder>();
        if (opened->open(*candidate.vtbl, path.c_str()) == MP_OK) {
            decoder = candidate.desc->id;
            return opened;
        }
        why = opened->why();
    }
    if (why.empty()) {
        why = "every decoder that recognised it refused to open it";
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

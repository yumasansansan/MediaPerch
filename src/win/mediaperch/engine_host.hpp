// SPDX-License-Identifier: GPL-3.0-or-later
//
// The four things the engine asks Windows for.
//
// `mp::Player` is portable and knows nothing about LoadLibrary, MMDevice or
// paths with drive letters. This is where all of that is, and it is small on
// purpose: if the Linux head is more than this file's worth of work, the split
// was drawn in the wrong place.

#ifndef MEDIAPERCH_WIN_ENGINE_HOST_HPP
#define MEDIAPERCH_WIN_ENGINE_HOST_HPP

#include "mediaperch/log.hpp"
#include "mediaperch/platform.hpp"
#include "mediaperch/player.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mp::win {

class EngineHost final : public IEngineHost {
public:
    EngineHost(const ModuleRegistry& registry, LogRing& log)
        : registry_(&registry), log_(&log)
    {
    }

    std::unique_ptr<ISource> open_source(const std::string& path, std::string& decoder,
                                         std::string& why) override;

    /// The same resolution with one demuxer named, which then gets no fallback:
    /// "use that one" answered with a different one is not an answer.
    ///
    /// The probe and the engine share this on purpose: two ways of opening a
    /// file that were not the same way would be worse than either.
    std::unique_ptr<ISource> open_source(const std::string& path, std::string_view prefer,
                                         std::string& decoder, std::string& why);
    Sink open_sink(const std::string& want, bool shared, std::string& resolved,
                   std::string& why) override;
    [[nodiscard]] const MpDspVtbl* dsp(const std::string& id) override;
    [[nodiscard]] bool device_ready(const std::string& want, bool shared) override;
    void log(const std::string& line) override;

    /// Whether an endpoint whose name contains `want` exists, and which one.
    /// Empty `want` is the default endpoint, whose id is the empty string.
    [[nodiscard]] bool resolve(const std::string& want, std::string& id, std::string& name,
                               std::string& why) const;

    /// Container readers to move to the front of the ranked list, in this
    /// order. How somebody says "read MP4s with FFmpeg" without arguing with a
    /// probe that is right about everything else -- and a reordering rather
    /// than a veto, so a module named here that does not recognise a file
    /// leaves the file to whoever does.
    void prefer(std::vector<std::string> modules) { prefer_ = std::move(modules); }

private:
    const ModuleRegistry* registry_;
    LogRing* log_;
    std::vector<std::string> prefer_;
};

} // namespace mp::win

#endif // MEDIAPERCH_WIN_ENGINE_HOST_HPP

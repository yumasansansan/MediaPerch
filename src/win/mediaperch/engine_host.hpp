// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seven things the engine asks Windows for.
//
// `mp::Player` is portable and knows nothing about LoadLibrary, MMDevice or
// paths with drive letters. This is where all of that is, and it is small on
// purpose: if the Linux head is more than this file's worth of work, the split
// was drawn in the wrong place.

#ifndef MEDIAPERCH_WIN_ENGINE_HOST_HPP
#define MEDIAPERCH_WIN_ENGINE_HOST_HPP

#include "mediaperch/display_win.hpp"
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

    std::unique_ptr<IMedia> open_media(const std::string& path, std::string& why) override;

    /// The same resolution with one demuxer named, which then gets no fallback:
    /// "use that one" answered with a different one is not an answer.
    ///
    /// The probe and the engine share this on purpose: two ways of opening a
    /// file that were not the same way would be worse than either. It is also
    /// why there is no `open_source` beside it any more -- the commands that
    /// want only the audio take `IMedia::audio()`, which is the same file
    /// opened the same way.
    std::unique_ptr<IMedia> open_media(const std::string& path, std::string_view prefer,
                                       std::string& why);
    Sink open_sink(const std::string& want, bool shared, std::string& resolved,
                   std::string& why) override;
    /// §9.7.1's presenter. **A null window is not off-screen here**: it is a
    /// composition surface a shell composites, because that is what a
    /// windowless engine's picture is for. A measurement that wants a texture
    /// nobody shows asks the module directly, which is what the tests do.
    std::unique_ptr<Presenter> open_presenter(void* window, std::string& module,
                                              std::string& why) override;
    std::unique_ptr<VideoDecoder> open_video_decoder(MpCodec codec,
                                                     const MpGraphicsDevice* device,
                                                     const std::uint8_t* config,
                                                     std::uint32_t config_bytes,
                                                     std::string& module,
                                                     std::string& why) override;
    [[nodiscard]] std::unique_ptr<IFrameClock> frame_clock(Presenter& presenter,
                                                           void* window) override;
    [[nodiscard]] const MpDspVtbl* dsp(const std::string& id) override;
    [[nodiscard]] std::vector<ipc::ModuleRow> modules() override;
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

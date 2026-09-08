// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the video engine needs from the operating system it happens to be on.
//
// **Four doors, and no audio behind any of them.** Which presenter module is
// loaded and which decoder claims a codec is a registry's business, and a
// registry is a `LoadLibrary` away from being portable; what to do with what
// comes back -- hand the presenter's device to the decoder, build the graph,
// run the loop -- is `mp::VideoPath`, and that is here. A product that wants
// the picture and not the sound implements this and links `MediaPerch::video`;
// `IEngineHost`, the player's, is this plus the audio half plus opening files.

#ifndef MEDIAPERCH_VIDEO_HOST_HPP
#define MEDIAPERCH_VIDEO_HOST_HPP

#include "mediaperch/display.hpp"
#include "mediaperch/video.hpp"

#include <mediaperch/module.h>

#include <cstdint>
#include <memory>
#include <string>

namespace mp {

class IVideoHost {
public:
    IVideoHost() = default;
    IVideoHost(const IVideoHost&) = delete;
    IVideoHost& operator=(const IVideoHost&) = delete;
    IVideoHost(IVideoHost&&) = delete;
    IVideoHost& operator=(IVideoHost&&) = delete;
    virtual ~IVideoHost() = default;

    /// A video stage by module id (§9.8.3), or nullptr. **A separate door
    /// because it is a separate vtable**, not because finding a module is a
    /// different problem: one that answered `void*` would be a door that had
    /// given up on saying what it returns.
    [[nodiscard]] virtual const MpVideoDspVtbl* video_dsp(const std::string&)
    {
        return nullptr;
    }

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
};

} // namespace mp

#endif // MEDIAPERCH_VIDEO_HOST_HPP

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace mp {

class IPacketFeed;

/// Where bytes come from, as the graph sees it.
///
/// A decoder module is wrapped in one of these; so is the tone generator. It
/// reports one format and never converts -- conversion is the graph's job, and
/// in the passthrough graph there is barely any.
class ISource {
public:
    ISource() = default;
    ISource(const ISource&) = delete;
    ISource& operator=(const ISource&) = delete;
    ISource(ISource&&) = delete;
    ISource& operator=(ISource&&) = delete;
    virtual ~ISource() = default;

    [[nodiscard]] virtual const Format& format() const noexcept = 0;

    /// Fills at most `bytes`, always a whole number of frames. Returns how much
    /// was written; 0 means the end of the stream.
    ///
    /// Called from the decode thread, so it may block and allocate.
    virtual std::size_t read(void* dst, std::size_t bytes) = 0;

    /// Whether `seek` will do anything. A tone generator has no position to go
    /// to; a file does.
    [[nodiscard]] virtual bool seekable() const noexcept { return false; }

    /// Moves to `frame`, counted from the start in this source's own frames.
    /// Returns false when it could not, which a caller must not treat as
    /// "probably worked": the position afterwards is then unknown.
    virtual bool seek(std::uint64_t /*frame*/) { return false; }

    /// Total frames, or 0 where the source does not know -- a stream, or a tone
    /// that goes on until somebody stops it.
    [[nodiscard]] virtual std::uint64_t length_frames() const noexcept { return 0; }
};

/// One file, opened once, with everything a player wants coming out of it.
///
/// **§4 as a type.** A file has one position. A host that handed back an audio
/// source and left the caller to open the video separately would be handing
/// back two demuxers and two positions, and a seek would then have to move both
/// and land them on the same moment -- which the ABI header already says at
/// length is the thing to avoid. So the file is opened once, `PacketRouter`
/// owns the position, and this is what comes back.
///
/// **The halves are not symmetrical, and that is deliberate.** The audio is an
/// `ISource` because the graphs take one and because the audio device is §8's
/// master clock, so it must be playing before anything else is decided. The
/// video is a *feed* and three facts about the stream, because a video decoder
/// is opened against a presenter's graphics device (§9.8.1) and the host has no
/// presenter -- whoever has one opens it, which is `mp::VideoPath`.
class IMedia {
public:
    IMedia() = default;
    IMedia(const IMedia&) = delete;
    IMedia& operator=(const IMedia&) = delete;
    IMedia(IMedia&&) = delete;
    IMedia& operator=(IMedia&&) = delete;
    virtual ~IMedia() = default;

    /// The audio. A file with none is not opened at all, so this is never a
    /// source that reads nothing -- **a video-only file is a skipped entry**,
    /// for the reason §8 gives: the audio device is the clock, and a playlist
    /// entry with no clock is a different kind of thing from the others.
    [[nodiscard]] virtual ISource& audio() noexcept = 0;

    /// The picture's packets, or null when there is none.
    ///
    /// Null covers three cases that are the same from here: the file has no
    /// video track, nothing loaded decodes it, or the caller asked for none.
    /// A player with a null feed plays the audio and is not in an error state.
    [[nodiscard]] virtual IPacketFeed* video() noexcept { return nullptr; }

    /// What the video stream is, for whoever opens a decoder and a presenter.
    /// Meaningless when `video()` is null.
    struct Picture {
        MpVideoInfo info{};
        MpCodec codec = MP_CODEC_UNKNOWN;
        /// The container's blob, verbatim -- `avcC` for H.264 in MP4. Valid as
        /// long as the media is; empty when the container carries none.
        const std::uint8_t* config = nullptr;
        std::uint32_t config_bytes = 0;
    };
    [[nodiscard]] virtual Picture picture() const noexcept { return {}; }

    /// Which module decoded the audio, for the report.
    [[nodiscard]] virtual const std::string& decoder() const noexcept
    {
        static const std::string none;
        return none;
    }
};

/// Where a queue gets its next source.
///
/// A callback rather than a list, because *when a file is opened* is the host's
/// business and not the queue's: opening five hundred decoders because a
/// playlist has five hundred entries is a decision somebody else should make.
class IPlaylist {
public:
    IPlaylist() = default;
    IPlaylist(const IPlaylist&) = delete;
    IPlaylist& operator=(const IPlaylist&) = delete;
    IPlaylist(IPlaylist&&) = delete;
    IPlaylist& operator=(IPlaylist&&) = delete;
    virtual ~IPlaylist() = default;

    /// The source at `index`, or nullptr past the end. Called from the decode
    /// thread, at the moment the previous one runs out, so it may open a file.
    [[nodiscard]] virtual ISource* at(std::size_t index) = 0;
};

/// What the render thread needs from the platform and the core cannot provide.
///
/// The graph creates the thread -- `std::thread` is portable -- but joining
/// MMCSS `Pro Audio` is not, and it has to happen on that thread rather than on
/// the one that created it.
class IRenderThreadHooks {
public:
    IRenderThreadHooks() = default;
    IRenderThreadHooks(const IRenderThreadHooks&) = delete;
    IRenderThreadHooks& operator=(const IRenderThreadHooks&) = delete;
    IRenderThreadHooks(IRenderThreadHooks&&) = delete;
    IRenderThreadHooks& operator=(IRenderThreadHooks&&) = delete;
    virtual ~IRenderThreadHooks() = default;

    /// First thing the render thread does.
    virtual void enter() noexcept = 0;
    /// Last thing it does, including when it is stopping because of an error.
    virtual void leave() noexcept = 0;
};

} // namespace mp

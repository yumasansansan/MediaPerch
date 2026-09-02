// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <cstddef>
#include <cstdint>

namespace mp {

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
    virtual bool seek(std::uint64_t frame) { return false; }

    /// Total frames, or 0 where the source does not know -- a stream, or a tone
    /// that goes on until somebody stops it.
    [[nodiscard]] virtual std::uint64_t length_frames() const noexcept { return 0; }
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

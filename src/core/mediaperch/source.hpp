// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"

#include <cstddef>

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

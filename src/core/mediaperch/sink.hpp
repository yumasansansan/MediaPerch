// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"
#include "mediaperch/negotiation.hpp"

#include <cstdint>

namespace mp {

/// A sink module, held by the graph.
///
/// Owns the handle and closes it. Every call forwards straight to the C vtable;
/// the three marked MP_RT are the ones the render thread uses and they add
/// nothing of their own -- no allocation, no locking, no logging.
class Sink {
public:
    Sink() noexcept = default;
    Sink(const MpSinkVtbl* vtbl, MpSink* handle) noexcept : vtbl_(vtbl), handle_(handle) {}

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&& other) noexcept;
    Sink& operator=(Sink&& other) noexcept;
    ~Sink() { close(); }

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    /// Really initialises the device. May fail with MP_ERR_FORMAT, which is a
    /// normal outcome and the reason §6 exists.
    MpResult negotiate(const Format& want, Format& accepted) noexcept;

    /// Frames in one buffer period. Valid only after a successful negotiate.
    MpResult period_frames(std::uint32_t& frames) noexcept;

    MpResult start() noexcept;
    MpResult stop() noexcept;

    MpResult wait(std::uint32_t timeout_ms) noexcept;              ///< MP_RT
    MpResult acquire(void*& ptr, std::uint32_t& frames) noexcept;  ///< MP_RT
    MpResult commit(std::uint32_t frames, std::uint32_t flags) noexcept; ///< MP_RT

    /// The master clock: frames played, and the QPC tick it was read at.
    MpResult position(std::uint64_t& frames, std::uint64_t& qpc) noexcept;

    void close() noexcept;

private:
    const MpSinkVtbl* vtbl_ = nullptr;
    MpSink* handle_ = nullptr;
};

/// What §6 produced, and how it got there.
struct Negotiated {
    bool ok = false;
    Format accepted{};
    Fidelity fidelity = Fidelity::converted;
    bool channel_mask_added = false;
    /// How many candidates were offered before one stuck. Worth logging: a
    /// device that takes the fourth is telling you something about its driver.
    std::size_t tried = 0;
    /// The last thing the sink said when nothing worked.
    MpResult last_error = MP_OK;
};

/// Walk the candidate list and stop at the first format the sink really accepts.
///
/// The returned format is checked against the source with `classify` rather than
/// taken on trust. A sink that answers MP_OK while handing back something that
/// is not bit-exact has failed, not succeeded, and treating that as success
/// would quietly defeat the one property this program exists to have.
[[nodiscard]] Negotiated negotiate_best(Sink& sink, const Format& source);

} // namespace mp

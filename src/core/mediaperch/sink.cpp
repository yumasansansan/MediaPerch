// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/sink.hpp"

#include <utility>

namespace mp {

Sink::Sink(Sink&& other) noexcept
    : vtbl_(std::exchange(other.vtbl_, nullptr)), handle_(std::exchange(other.handle_, nullptr))
{
}

Sink& Sink::operator=(Sink&& other) noexcept
{
    if (this != &other) {
        close();
        vtbl_ = std::exchange(other.vtbl_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

void Sink::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
}

MpResult Sink::negotiate(const Format& want, Format& accepted) noexcept
{
    if (!*this || vtbl_->negotiate == nullptr) {
        return MP_ERR_INVALID;
    }
    const MpFormat request = to_abi(want);
    MpFormat got{};
    const MpResult r = vtbl_->negotiate(handle_, &request, &got);
    if (r == MP_OK) {
        accepted = from_abi(got);
    }
    return r;
}

MpResult Sink::period_frames(std::uint32_t& frames) noexcept
{
    if (!*this || vtbl_->get_period == nullptr) {
        return MP_ERR_INVALID;
    }
    return vtbl_->get_period(handle_, &frames);
}

MpResult Sink::start() noexcept
{
    if (!*this || vtbl_->start == nullptr) {
        return MP_ERR_INVALID;
    }
    return vtbl_->start(handle_);
}

MpResult Sink::stop() noexcept
{
    if (!*this || vtbl_->stop == nullptr) {
        return MP_ERR_INVALID;
    }
    return vtbl_->stop(handle_);
}

MpResult Sink::wait(std::uint32_t timeout_ms) noexcept
{
    return vtbl_->wait(handle_, timeout_ms);
}

MpResult Sink::acquire(void*& ptr, std::uint32_t& frames) noexcept
{
    return vtbl_->acquire(handle_, &ptr, &frames);
}

MpResult Sink::commit(std::uint32_t frames, std::uint32_t flags) noexcept
{
    return vtbl_->commit(handle_, frames, flags);
}

MpResult Sink::position(std::uint64_t& frames, std::uint64_t& qpc) noexcept
{
    if (!*this || vtbl_->get_position == nullptr) {
        return MP_ERR_INVALID;
    }
    return vtbl_->get_position(handle_, &frames, &qpc);
}

Negotiated negotiate_best(Sink& sink, const Format& source)
{
    Negotiated out;
    if (!sink) {
        out.last_error = MP_ERR_INVALID;
        return out;
    }

    for (const Candidate& candidate : build_candidates(source)) {
        ++out.tried;

        Format accepted{};
        const MpResult r = sink.negotiate(candidate.format, accepted);
        if (r != MP_OK) {
            out.last_error = r;
            continue;
        }

        // Do not take the sink's word for it. A driver that accepts a format and
        // hands back a different one has failed the request, and calling that
        // success is exactly how a player ends up quietly resampling.
        const Fidelity actual = classify(source, accepted);
        if (!is_bit_exact(actual)) {
            out.last_error = MP_ERR_FORMAT;
            continue;
        }

        out.ok = true;
        out.accepted = accepted;
        out.fidelity = actual;
        out.channel_mask_added = candidate.channel_mask_added;
        out.last_error = MP_OK;
        return out;
    }

    if (out.tried == 0) {
        out.last_error = MP_ERR_INVALID; // is_valid rejected the source
    } else if (out.last_error == MP_OK) {
        out.last_error = MP_ERR_FORMAT;
    }
    return out;
}

} // namespace mp

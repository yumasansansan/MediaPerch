// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/video.hpp"

namespace mp {

// --------------------------------------------------------------------------
// VideoDecoder
// --------------------------------------------------------------------------

MpResult VideoDecoder::open(const MpVideoCodecVtbl& vtbl, MpCodec codec,
                            const MpGraphicsDevice* device, const std::uint8_t* config,
                            std::uint32_t config_bytes)
{
    close();
    if (vtbl.open == nullptr) {
        return MP_ERR_INVALID;
    }
    MpVideoCodec* handle = nullptr;
    const MpResult r = vtbl.open(codec, device, config, config_bytes, &handle);
    if (r != MP_OK || handle == nullptr) {
        return r != MP_OK ? r : MP_ERR_INTERNAL;
    }
    vtbl_ = &vtbl;
    handle_ = handle;
    return MP_OK;
}

void VideoDecoder::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
}

MpResult VideoDecoder::decode(const void* packet, std::size_t bytes,
                              std::uint64_t pts) noexcept
{
    if (!*this) {
        return MP_ERR_INVALID;
    }
    return vtbl_->decode(handle_, packet, bytes, pts);
}

MpResult VideoDecoder::next_frame(MpVideoFrame& out) noexcept
{
    if (!*this) {
        return MP_ERR_INVALID;
    }
    out = MpVideoFrame{};
    out.size = sizeof(MpVideoFrame);
    return vtbl_->next_frame(handle_, &out);
}

MpResult VideoDecoder::get_format(MpVideoInfo& out) noexcept
{
    if (!*this) {
        return MP_ERR_INVALID;
    }
    out = MpVideoInfo{};
    out.size = sizeof(MpVideoInfo);
    return vtbl_->get_format(handle_, &out);
}

MpResult VideoDecoder::flush() noexcept
{
    return *this ? vtbl_->flush(handle_) : MP_ERR_INVALID;
}

MpResult VideoDecoder::reset() noexcept
{
    return *this ? vtbl_->reset(handle_) : MP_ERR_INVALID;
}

// --------------------------------------------------------------------------
// Presenter
// --------------------------------------------------------------------------

MpResult Presenter::open(const MpVideoVtbl& vtbl, void* window)
{
    close();
    if (vtbl.open == nullptr) {
        return MP_ERR_INVALID;
    }
    MpVideo* handle = nullptr;
    const MpResult r = vtbl.open(window, &handle);
    if (r != MP_OK || handle == nullptr) {
        return r != MP_OK ? r : MP_ERR_INTERNAL;
    }
    vtbl_ = &vtbl;
    handle_ = handle;
    return MP_OK;
}

void Presenter::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
}

MpResult Presenter::configure(const MpVideoInfo& info) noexcept
{
    return *this ? vtbl_->configure(handle_, &info) : MP_ERR_INVALID;
}

MpResult Presenter::present(const MpVideoFrame& frame) noexcept
{
    return *this ? vtbl_->present(handle_, &frame) : MP_ERR_INVALID;
}

MpResult Presenter::set(const char* key, const char* value) noexcept
{
    return *this ? vtbl_->set(handle_, key, value) : MP_ERR_INVALID;
}

MpResult Presenter::get_device(MpGraphicsDevice& out) noexcept
{
    if (!*this || vtbl_->get_device == nullptr) {
        return MP_ERR_INVALID;
    }
    out = MpGraphicsDevice{};
    out.size = sizeof(MpGraphicsDevice);
    return vtbl_->get_device(handle_, &out);
}

MpResult Presenter::read_back(void* dst, std::size_t dst_bytes, std::uint32_t& width,
                              std::uint32_t& height, MpPixelLayout& layout) noexcept
{
    if (!*this || vtbl_->read_back == nullptr) {
        return MP_ERR_INVALID;
    }
    layout = MpPixelLayout{};
    layout.size = sizeof(MpPixelLayout);
    return vtbl_->read_back(handle_, dst, dst_bytes, &width, &height, &layout);
}

// --------------------------------------------------------------------------
// VideoGraph
// --------------------------------------------------------------------------

VideoGraph::VideoGraph(IPacketFeed& packets, VideoDecoder& decoder, Presenter& presenter,
                       const MpVideoInfo& info)
    : packets_(&packets), decoder_(&decoder), presenter_(&presenter), info_(info)
{
    pacer_.configure(info_.timescale, info_.fps_num, info_.fps_den);
}

MpResult VideoGraph::fetch()
{
    // Bounded, because every branch below either produces a frame, consumes a
    // packet or ends the stream -- and a decoder that did none of those would
    // otherwise spin here rather than say so.
    for (int guard = 0; guard < 4096; ++guard) {
        MpVideoFrame frame{};
        const MpResult got = decoder_->next_frame(frame);
        if (got == MP_OK) {
            frame_ = frame;
            ++stats_.decoded;
            return MP_OK;
        }
        if (got == MP_ERR_BUSY) {
            // The stream changed and the decoder renegotiated. codec_mft does
            // this on its first frame, always: the geometry comes out of the
            // bitstream rather than out of the container.
            continue;
        }
        if (got != MP_END) {
            return got;
        }
        if (drained_) {
            return MP_END;
        }

        const MpResult read = packets_->next(buffer_, packet_);
        if (read == MP_END) {
            // No more packets. What the decoder is holding back to reorder is
            // the tail of the file, and a caller that stopped at the last
            // packet would lose it.
            const MpResult flushed = decoder_->flush();
            if (flushed != MP_OK) {
                return flushed;
            }
            drained_ = true;
            continue;
        }
        if (read != MP_OK) {
            return read;
        }
        const MpResult fed =
            decoder_->decode(buffer_.data(), packet_.bytes, packet_.frame);
        if (fed != MP_OK) {
            return fed;
        }
    }
    return MP_ERR_INTERNAL;
}

void VideoGraph::reconcile()
{
    reconciled_ = true;
    MpVideoInfo said{};
    if (decoder_->get_format(said) != MP_OK) {
        // libaom and avm cannot answer before a frame and dav1d can; a decoder
        // that will not answer at all is one whose container description is
        // the only description there is, which is what was configured.
        return;
    }

    // **The bitstream wins where it speaks.** A container states colour and a
    // stream states colour, and §9.1 says the container is what a renderer must
    // not guess -- but where the two disagree the pixels came from the one that
    // encoded them. Geometry the same way: `codec_mft` learns the real size
    // from the sequence parameter set.
    MpVideoInfo want = info_;
    bool changed = false;
    const auto take = [&changed](std::uint32_t& into, std::uint32_t from) {
        if (from != 0 && into != from) {
            into = from;
            changed = true;
        }
    };
    take(want.width, said.width);
    take(want.height, said.height);
    take(want.display_width, said.display_width);
    take(want.display_height, said.display_height);
    take(want.primaries, said.primaries);
    take(want.transfer, said.transfer);
    take(want.matrix, said.matrix);
    if (want.flags != said.flags) {
        want.flags = said.flags;
        changed = true;
    }
    // Timescale is the exception and the codecs say so in as many words: zero
    // there means "unchanged", because a decoder does not re-time a stream.
    // The frame rate is the container's for the same reason.

    if (changed && presenter_->configure(want) == MP_OK) {
        info_ = want;
    }
}

VideoGraph::Step VideoGraph::pump(double audible_seconds)
{
    if (finished_) {
        return Step::finished;
    }
    if (error_ != MP_OK) {
        return Step::failed;
    }

    for (;;) {
        if (!have_frame_) {
            const MpResult got = fetch();
            if (got == MP_END) {
                finished_ = true;
                return Step::finished;
            }
            if (got != MP_OK) {
                error_ = got;
                return Step::failed;
            }
            have_frame_ = true;
            if (!reconciled_) {
                reconcile();
            }
        }

        const VideoPacer::Decision decision = pacer_.decide(frame_.pts, audible_seconds);
        if (decision.fate == FrameFate::repeat) {
            // The frame stays in hand. Nothing is presented, and the picture
            // already up is the one that stays -- there is no work to do to
            // duplicate it, which is why there is no call here.
            return Step::repeated;
        }

        have_frame_ = false;
        if (decision.fate == FrameFate::drop) {
            ++stats_.dropped;
            // **Straight on to the next one**, on this same call. A decoder
            // that fell behind has several past frames to let go, and letting
            // one go per display refresh would never catch the clock.
            continue;
        }

        const MpResult shown = presenter_->present(frame_);
        if (shown != MP_OK) {
            error_ = shown;
            return Step::failed;
        }
        ++stats_.shown;
        if (decision.error_seconds < stats_.worst_late_seconds) {
            stats_.worst_late_seconds = decision.error_seconds;
        }
        return Step::shown;
    }
}

} // namespace mp

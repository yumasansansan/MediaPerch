// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/decoder.hpp"

#include <vector>

namespace mp {

MpResult Decoder::open(const MpDecoderVtbl& vtbl, const char* path)
{
    close();
    why_.clear();

    if (vtbl.size < sizeof(MpDecoderVtbl) || vtbl.open == nullptr ||
        vtbl.get_format == nullptr || vtbl.read == nullptr) {
        return MP_ERR_INVALID;
    }

    MpDecoder* handle = nullptr;
    const MpResult opened = vtbl.open(path, &handle);
    if (opened != MP_OK || handle == nullptr) {
        return opened == MP_OK ? MP_ERR_INTERNAL : opened;
    }

    MpFormat format{};
    const MpResult described = vtbl.get_format(handle, &format);
    if (described != MP_OK) {
        vtbl.close(handle);
        return described;
    }

    vtbl_ = &vtbl;
    handle_ = handle;
    format_ = from_abi(format);

    length_ = 0;
    if (vtbl.get_length != nullptr) {
        vtbl.get_length(handle, &length_);
    }

    // A decoder that reports a format no sink could be asked for has failed at
    // the one thing it is for, and finding that out here beats finding it out on
    // the render thread.
    if (!is_valid(format_)) {
        close();
        return MP_ERR_FORMAT;
    }

    // And a decoder that opens a file, describes it correctly, and then produces
    // nothing at all is the worst failure of the three, because it is
    // indistinguishable from an empty file: `read` returns 0, the graph calls
    // that the end of the stream, and the track is skipped without a word.
    //
    // Measured, not imagined: `dr_flac` opens a 32-bit FLAC, reports 32 bits
    // from STREAMINFO, and decodes zero frames -- its frame-header table still
    // marks the bit-depth code that FLAC 1.4 assigned to 32 bits as reserved.
    // The file plays as silence of length zero.
    if (!can_actually_decode()) {
        why_ = "the decoder opened the file and produced no audio at all";
        close();
        return MP_ERR_IO;
    }
    return MP_OK;
}

bool Decoder::can_actually_decode()
{
    // A decoder that cannot seek would lose the frame this reads, so the check
    // is skipped rather than made destructive. Every decoder here can seek.
    if (length_ == 0 || vtbl_->seek == nullptr) {
        return true;
    }

    std::vector<std::uint8_t> probe(frame_bytes(format_));
    if (probe.empty()) {
        return false;
    }

    std::size_t got = 0;
    const MpResult r = vtbl_->read(handle_, probe.data(), probe.size(), &got);
    const bool produced = (r == MP_OK || r == MP_END) && got > 0;

    vtbl_->seek(handle_, 0);
    return produced;
}

void Decoder::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
    format_ = Format{};
    length_ = 0;
}

std::size_t Decoder::read(void* dst, std::size_t bytes)
{
    if (!*this) {
        return 0;
    }
    std::size_t got = 0;
    const MpResult r = vtbl_->read(handle_, dst, bytes, &got);
    if (r != MP_OK && r != MP_END) {
        return 0;
    }
    return got;
}

MpResult Decoder::seek(std::uint64_t frame) noexcept
{
    if (!*this || vtbl_->seek == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->seek(handle_, frame);
}

} // namespace mp

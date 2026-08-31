// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mediaperch/format.hpp"
#include "mediaperch/source.hpp"

#include <cstdint>
#include <string>

namespace mp {

/// A decoder module, seen by the graph as a source.
///
/// Thin on purpose. The decoder reports one format and hands over the file's own
/// bytes; everything about what to do with them belongs to negotiation and the
/// graph, and none of it belongs here.
class Decoder final : public ISource {
public:
    Decoder() noexcept = default;
    ~Decoder() override { close(); }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    /// `path` is UTF-8, as the ABI says.
    MpResult open(const MpDecoderVtbl& vtbl, const char* path);
    void close() noexcept;

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    [[nodiscard]] const Format& format() const noexcept override { return format_; }

    /// Returns 0 at the end of the stream, as `ISource` requires.
    std::size_t read(void* dst, std::size_t bytes) override;

    MpResult seek(std::uint64_t frame) noexcept;

    /// Total frames, or 0 when the decoder does not know.
    [[nodiscard]] std::uint64_t length_frames() const noexcept { return length_; }

    /// Why `open` refused, in words, or empty. Set only for the failures a
    /// caller could not work out from the result code.
    [[nodiscard]] const std::string& why() const noexcept { return why_; }

private:
    /// Decodes one frame and rewinds. Catches a decoder that opens a file,
    /// describes it correctly and then produces nothing.
    [[nodiscard]] bool can_actually_decode();

    const MpDecoderVtbl* vtbl_ = nullptr;
    MpDecoder* handle_ = nullptr;
    Format format_{};
    std::uint64_t length_ = 0;
    std::string why_;
};

} // namespace mp

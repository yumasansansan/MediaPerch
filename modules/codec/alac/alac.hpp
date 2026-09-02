// SPDX-License-Identifier: GPL-3.0-or-later
//
// ALAC, decoded here rather than by somebody else's library.
//
// The reasoning is in §7 of docs/plan.md and it is short: ALAC's reference
// implementation is the specification *and* has been unmaintained since 2011,
// and those two facts point in opposite directions. Apple's code is where the
// format is defined, and it is also the code that gave Qualcomm and MediaTek
// CVE-2021-30351 and friends when they shipped it. Reading it to learn the
// format is right; linking it is not.
//
// So this is a fresh implementation, written from that source as a
// specification. What it does not inherit is the part that matters: every read
// here is bounds-checked, every value that comes out of the bitstream is
// validated before it is used as a length or a shift, and the buffers are sized
// once from a config that was checked first. Several of the checks below have a
// comment naming the specific thing the reference does not check.
//
// No OS headers, no allocation during decode, no I/O: this file is the codec
// and nothing else, which is what lets fuzz/alac_fuzzer.cpp drive it directly.

#ifndef MEDIAPERCH_ALAC_HPP
#define MEDIAPERCH_ALAC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp::alac {

/// The ALACSpecificConfig magic cookie, as it appears in an `alac` box.
struct Config {
    std::uint32_t frame_length = 0;
    std::uint8_t compatible_version = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t pb = 0;
    std::uint8_t mb = 0;
    std::uint8_t kb = 0;
    std::uint8_t channels = 0;
    std::uint16_t max_run = 0;
    std::uint32_t max_frame_bytes = 0;
    std::uint32_t avg_bit_rate = 0;
    std::uint32_t sample_rate = 0;
};

/// The cookie is 24 bytes big-endian. Anything shorter, or describing a stream
/// this decoder will not touch, is rejected here rather than deeper in.
bool parse_config(const std::uint8_t* cookie, std::size_t bytes, Config& out) noexcept;

/// Channels in the order ALAC stores them, which is Apple's and not WAVE's.
/// Index by channel count; index 0 is unused. Values are MP_SPEAKER_* bits.
///
/// This is the table Media Foundation forgets to apply -- see docs/formats.md.
struct ChannelLayout {
    std::uint32_t mask;
    std::uint8_t from[8]; ///< for each WAVE slot, the ALAC channel that fills it
};

const ChannelLayout& layout_for(unsigned channels) noexcept;

class Decoder {
public:
    /// Sizes every buffer from `cfg`. False if the config is not one this
    /// decoder will accept.
    bool init(const Config& cfg) noexcept;

    /// Decodes one packet. `out` receives `channels()` * result samples,
    /// interleaved, sign-extended and right-justified at `bit_depth()`, in
    /// **ALAC channel order**. Returns 0 on any error, and `error()` says which.
    ///
    /// `out` must have room for channels() * frame_length() samples.
    std::uint32_t decode(const std::uint8_t* packet, std::size_t bytes,
                         std::int32_t* out) noexcept;

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] unsigned channels() const noexcept { return cfg_.channels; }
    [[nodiscard]] unsigned bit_depth() const noexcept { return cfg_.bit_depth; }
    [[nodiscard]] std::uint32_t frame_length() const noexcept { return cfg_.frame_length; }
    [[nodiscard]] const char* error() const noexcept { return error_; }

private:
    Config cfg_{};
    const char* error_ = "";
    std::vector<std::int32_t> predictor_;
    std::vector<std::int32_t> mix_u_;
    std::vector<std::int32_t> mix_v_;
    std::vector<std::uint16_t> shift_;
    bool ready_ = false;
};

/// The largest frame this decoder will size buffers for. ALAC's own default is
/// 4096 and its encoder has no way to ask for more; a cookie claiming tens of
/// millions is a cookie asking us to allocate on its say-so.
inline constexpr std::uint32_t k_max_frame_length = 65536;
inline constexpr unsigned k_max_channels = 8;

} // namespace mp::alac

#endif // MEDIAPERCH_ALAC_HPP

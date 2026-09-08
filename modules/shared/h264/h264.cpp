// SPDX-License-Identifier: GPL-3.0-or-later
#include "h264.hpp"

#include <cstring>

namespace mp::mft {

namespace {

/// A reader that cannot run off the end, because every field below is a length
/// read out of a file somebody else wrote.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t bytes) : p_(data), left_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& out)
    {
        if (left_ < 1) {
            return false;
        }
        out = *p_++;
        --left_;
        return true;
    }
    [[nodiscard]] bool u16(std::uint32_t& out)
    {
        std::uint8_t hi = 0;
        std::uint8_t lo = 0;
        if (!u8(hi) || !u8(lo)) {
            return false;
        }
        out = (static_cast<std::uint32_t>(hi) << 8) | lo;
        return true;
    }
    [[nodiscard]] bool take(std::size_t n, std::vector<std::uint8_t>& out)
    {
        if (left_ < n) {
            return false;
        }
        out.assign(p_, p_ + n);
        p_ += n;
        left_ -= n;
        return true;
    }
    [[nodiscard]] bool skip(std::size_t n)
    {
        if (left_ < n) {
            return false;
        }
        p_ += n;
        left_ -= n;
        return true;
    }

private:
    const std::uint8_t* p_;
    std::size_t left_;
};

constexpr std::uint8_t k_start_code[] = {0x00, 0x00, 0x00, 0x01};

/// A bit reader over an RBSP, which is a NAL unit with its emulation prevention
/// bytes taken out.
///
/// **The three-byte sequence is the whole reason this is not a plain bit
/// reader.** A NAL unit may not contain `00 00 00`, `00 00 01`, `00 00 02` or
/// `00 00 03`, because a start code scanner would trip over the first two -- so
/// an encoder inserts a `03` after any `00 00` that would otherwise be followed
/// by one of them, and a decoder takes it back out. Reading the bits without
/// removing it shifts everything after the first occurrence by eight, which
/// produces a plausible wrong answer rather than an error.
class Bits {
public:
    Bits(const std::uint8_t* data, std::size_t bytes) : p_(data), left_(bytes) {}

    /// One bit, or false at the end.
    [[nodiscard]] bool u1(std::uint32_t& out)
    {
        if (bit_ == 0 && !advance()) {
            return false;
        }
        out = (byte_ >> (bit_ - 1)) & 1u;
        --bit_;
        return true;
    }

    [[nodiscard]] bool un(std::uint32_t count, std::uint32_t& out)
    {
        out = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t bit = 0;
            if (!u1(bit)) {
                return false;
            }
            out = (out << 1) | bit;
        }
        return true;
    }

    /// **Bits, not bytes.** The byte reader above has a `skip` too and they
    /// mean different things; this one is what a syntax element is measured in.
    [[nodiscard]] bool skip(std::uint32_t count)
    {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t bit = 0;
            if (!u1(bit)) {
                return false;
            }
        }
        return true;
    }

    /// Unsigned Exp-Golomb: N zero bits, a one, then N more bits.
    [[nodiscard]] bool ue(std::uint32_t& out)
    {
        std::uint32_t zeros = 0;
        for (;;) {
            std::uint32_t bit = 0;
            if (!u1(bit)) {
                return false;
            }
            if (bit != 0) {
                break;
            }
            // **Refused rather than wrapped.** 32 leading zeros would need 33
            // bits of value, which no field in this syntax has and which a
            // corrupt file can ask for.
            if (++zeros > 31) {
                return false;
            }
        }
        std::uint32_t rest = 0;
        if (!un(zeros, rest)) {
            return false;
        }
        out = ((1u << zeros) - 1u) + rest;
        return true;
    }

private:
    /// The next byte, skipping an emulation prevention byte when the two before
    /// it were zero.
    [[nodiscard]] bool advance()
    {
        if (left_ == 0) {
            return false;
        }
        std::uint8_t next = *p_++;
        --left_;
        if (zeros_ >= 2 && next == 0x03) {
            if (left_ == 0) {
                return false; // a trailing 03 with nothing after it
            }
            next = *p_++;
            --left_;
            zeros_ = 0;
        }
        zeros_ = next == 0 ? zeros_ + 1 : 0;
        byte_ = next;
        bit_ = 8;
        return true;
    }

    const std::uint8_t* p_;
    std::size_t left_;
    std::uint8_t byte_ = 0;
    std::uint32_t bit_ = 0;
    std::uint32_t zeros_ = 0;
};

/// The profiles whose SPS carries the chroma format and the bit depths.
///
/// Every other profile infers 4:2:0 and eight bits, which the standard states
/// and which is why a Baseline stream has none of these fields to read.
bool carries_chroma_format(std::uint32_t profile_idc)
{
    switch (profile_idc) {
    case 100: // High
    case 110: // High 10
    case 122: // High 4:2:2
    case 244: // High 4:4:4 Predictive
    case 44:  // CAVLC 4:4:4 Intra
    case 83:  // Scalable Baseline
    case 86:  // Scalable High
    case 118: // Multiview High
    case 128: // Stereo High
    case 138: // Multiview Depth High
    case 139: // Enhanced Multiview Depth High
    case 134: // MFC High
    case 135: // MFC Depth High
        return true;
    default:
        return false;
    }
}

/// `scaling_list`, stepped over rather than kept: nothing here needs the
/// coefficients and the fields after it do.
[[nodiscard]] bool skip_scaling_list(Bits& in, std::uint32_t size)
{
    std::int32_t next = 8;
    std::int32_t last = 8;
    for (std::uint32_t i = 0; i < size; ++i) {
        if (next != 0) {
            std::uint32_t code = 0;
            if (!in.ue(code)) {
                return false;
            }
            // se(v): the unsigned code maps to a signed delta, and only its
            // effect on `next` matters here.
            const std::int32_t delta =
                (code & 1u) != 0 ? static_cast<std::int32_t>((code + 1u) / 2u)
                                 : -static_cast<std::int32_t>(code / 2u);
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
    return true;
}

} // namespace

AvcConfig parse_avcc(const std::uint8_t* data, std::size_t bytes)
{
    AvcConfig config{};
    if (data == nullptr) {
        return config;
    }
    Reader in{data, bytes};

    // configurationVersion, AVCProfileIndication, profile_compatibility,
    // AVCLevelIndication. All four are told to the decoder by the SPS as well,
    // so none of them is read here -- but they have to be stepped over.
    if (!in.skip(4)) {
        return config;
    }

    std::uint8_t packed = 0;
    if (!in.u8(packed)) {
        return config;
    }
    // Six reserved bits set to 1, then lengthSizeMinusOne.
    config.length_size = static_cast<std::uint32_t>(packed & 0x03u) + 1u;
    if (config.length_size == 3) {
        // The spec forbids 3, which means a lengthSizeMinusOne of 2. A file
        // that says it is a file to decline rather than to guess at.
        return config;
    }

    if (!in.u8(packed)) {
        return config;
    }
    const std::uint32_t sps_count = packed & 0x1Fu; // three reserved bits above
    for (std::uint32_t i = 0; i < sps_count; ++i) {
        std::uint32_t length = 0;
        std::vector<std::uint8_t> nal;
        if (!in.u16(length) || !in.take(length, nal)) {
            return config;
        }
        config.parameter_sets.push_back(std::move(nal));
    }

    std::uint8_t pps_count = 0;
    if (!in.u8(pps_count)) {
        return config;
    }
    for (std::uint32_t i = 0; i < pps_count; ++i) {
        std::uint32_t length = 0;
        std::vector<std::uint8_t> nal;
        if (!in.u16(length) || !in.take(length, nal)) {
            return config;
        }
        config.parameter_sets.push_back(std::move(nal));
    }

    // **An avcC with no parameter sets is not usable**, whatever else it says.
    // A decoder handed a stream with no SPS produces nothing and says little
    // about why.
    config.valid = !config.parameter_sets.empty();
    return config;
}

SpsInfo parse_sps(const std::uint8_t* nal, std::size_t bytes)
{
    SpsInfo out{};
    if (nal == nullptr || bytes < 4) {
        return out;
    }
    // The NAL header: a forbidden zero bit, two bits of nal_ref_idc, and five
    // bits of nal_unit_type. 7 is a sequence parameter set and nothing else is
    // one, so a PPS handed here is declined rather than read as an SPS.
    if ((nal[0] & 0x80u) != 0 || (nal[0] & 0x1Fu) != 7u) {
        return out;
    }

    Bits in{nal + 1, bytes - 1};
    std::uint32_t value = 0;

    if (!in.un(8, out.profile_idc)) {
        return out;
    }
    // constraint_set flags with two reserved bits, then level_idc.
    if (!in.un(8, value) || !in.un(8, value)) {
        return out;
    }
    if (!in.ue(value)) { // seq_parameter_set_id
        return out;
    }

    if (carries_chroma_format(out.profile_idc)) {
        if (!in.ue(out.chroma_format_idc) || out.chroma_format_idc > 3) {
            return out;
        }
        if (out.chroma_format_idc == 3) {
            if (!in.u1(value)) {
                return out;
            }
            out.separate_colour_plane = value != 0;
        }
        std::uint32_t luma = 0;
        std::uint32_t chroma = 0;
        if (!in.ue(luma) || !in.ue(chroma) || luma > 6 || chroma > 6) {
            return out;
        }
        // The fields are `minus8`, and the standard caps them at 6 -- fourteen
        // bits, which is the deepest H.264 goes.
        out.bit_depth_luma = 8 + luma;
        out.bit_depth_chroma = 8 + chroma;

        if (!in.u1(value)) { // qpprime_y_zero_transform_bypass_flag
            return out;
        }
        std::uint32_t scaling_present = 0;
        if (!in.u1(scaling_present)) {
            return out;
        }
        if (scaling_present != 0) {
            // Eight lists for 4:2:0 and 4:2:2, twelve for 4:4:4. Stepped over
            // because the fields after it are not read either -- but stepped
            // over correctly, so that a stream with scaling lists does not
            // silently become a stream this parser gave up on.
            const std::uint32_t lists = out.chroma_format_idc != 3 ? 8u : 12u;
            for (std::uint32_t i = 0; i < lists; ++i) {
                std::uint32_t present = 0;
                if (!in.u1(present)) {
                    return out;
                }
                if (present != 0 && !skip_scaling_list(in, i < 6 ? 16u : 64u)) {
                    return out;
                }
            }
        }
    }

    out.valid = true;
    return out;
}

SpsInfo sps_of(const AvcConfig& config)
{
    if (!config.valid) {
        return SpsInfo{};
    }
    for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
        const SpsInfo sps = parse_sps(nal.data(), nal.size());
        if (sps.valid) {
            return sps;
        }
    }
    return SpsInfo{};
}

HevcConfig parse_hvcc(const std::uint8_t* data, std::size_t bytes)
{
    HevcConfig config{};
    if (data == nullptr) {
        return config;
    }
    Reader in{data, bytes};

    // configurationVersion, then the profile space, tier and profile_idc packed
    // into one byte. The profile is worth keeping: it is the difference between
    // Main and Main 10 and a decoder that does only the first wants to know.
    std::uint8_t packed = 0;
    if (!in.skip(1) || !in.u8(packed)) {
        return config;
    }
    config.profile_idc = packed & 0x1Fu;

    // profile_compatibility_flags (4), constraint_indicator_flags (6),
    // level_idc (1), then the two reserved-and-packed bytes holding
    // min_spatial_segmentation_idc, and parallelismType. Eleven in all, none of
    // which a decoder is told anything by that the parameter sets do not repeat.
    if (!in.skip(4 + 6 + 1 + 2 + 1)) {
        return config;
    }

    // Six reserved bits then chroma_format_idc; five then each bit depth.
    if (!in.u8(packed)) {
        return config;
    }
    config.chroma_format_idc = packed & 0x03u;
    if (!in.u8(packed)) {
        return config;
    }
    config.bit_depth_luma = (packed & 0x07u) + 8u;
    if (!in.u8(packed)) {
        return config;
    }
    config.bit_depth_chroma = (packed & 0x07u) + 8u;

    // avgFrameRate, which the container states properly elsewhere and which is
    // zero in most files anyway.
    if (!in.skip(2)) {
        return config;
    }

    // constantFrameRate (2), numTemporalLayers (3), temporalIdNested (1), and
    // then the two bits that matter.
    if (!in.u8(packed)) {
        return config;
    }
    config.annex.length_size = static_cast<std::uint32_t>(packed & 0x03u) + 1u;
    if (config.annex.length_size == 3) {
        // Forbidden here for the reason it is forbidden in `avcC`.
        return config;
    }

    // **The arrays, which is where `hvcC` stops looking like `avcC`.** HEVC has
    // three kinds of parameter set rather than two -- a video parameter set
    // above the sequence and picture ones -- and they arrive grouped by kind
    // instead of in two counted lists. The order the arrays appear in is the
    // order to emit them, and files write VPS, SPS, PPS; nothing here reorders,
    // because a file that wrote them another way meant it.
    std::uint8_t arrays = 0;
    if (!in.u8(arrays)) {
        return config;
    }
    for (std::uint32_t a = 0; a < arrays; ++a) {
        // array_completeness (1), reserved (1), NAL_unit_type (6). The type is
        // not checked: emitting whatever the record holds, in the order it
        // holds it, is what a decoder wants, and a record carrying an SEI in
        // there is not this function's to object to.
        std::uint32_t count = 0;
        if (!in.skip(1) || !in.u16(count)) {
            return config;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t length = 0;
            std::vector<std::uint8_t> nal;
            if (!in.u16(length) || !in.take(length, nal)) {
                return config;
            }
            config.annex.parameter_sets.push_back(std::move(nal));
        }
    }

    config.annex.valid = !config.annex.parameter_sets.empty();
    config.valid = config.annex.valid;
    return config;
}

namespace {

/// A NAL with its emulation prevention bytes removed.
///
/// The bit reader above does this as it goes; an SEI payload is read a whole
/// byte at a time and in fixed-width fields, so it is cheaper to unescape once
/// than to carry the state through every read.
std::vector<std::uint8_t> unescaped(const std::uint8_t* nal, std::size_t bytes)
{
    std::vector<std::uint8_t> out;
    out.reserve(bytes);
    std::size_t zeros = 0;
    for (std::size_t at = 0; at < bytes; ++at) {
        const std::uint8_t byte = nal[at];
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue; // the escape itself, which is not data
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        out.push_back(byte);
    }
    return out;
}

std::uint32_t be16_at(const std::vector<std::uint8_t>& b, std::size_t at) noexcept
{
    return (static_cast<std::uint32_t>(b[at]) << 8) | b[at + 1];
}

std::uint32_t be32_at(const std::vector<std::uint8_t>& b, std::size_t at) noexcept
{
    return (static_cast<std::uint32_t>(b[at]) << 24) |
           (static_cast<std::uint32_t>(b[at + 1]) << 16) |
           (static_cast<std::uint32_t>(b[at + 2]) << 8) | b[at + 3];
}

} // namespace

namespace {

/// Signed Exp-Golomb, which `Bits` did not need until the scaling lists.
bool se(Bits& in, std::int32_t& out)
{
    std::uint32_t coded = 0;
    if (!in.ue(coded)) {
        return false;
    }
    // k maps to (-1)^(k+1) * ceil(k/2): 0, 1, -1, 2, -2 ...
    const std::uint32_t magnitude = (coded + 1u) / 2u;
    out = (coded & 1u) != 0 ? static_cast<std::int32_t>(magnitude)
                            : -static_cast<std::int32_t>(magnitude);
    return true;
}

/// `profile_tier_level`, whose only job here is to be skipped exactly.
///
/// **Eleven bytes and a level byte for the general layer**, then two flags per
/// sub-layer, then a padded byte, then the sub-layers themselves. Getting the
/// length wrong shifts everything after it and produces a plausible wrong
/// answer rather than a failure, which is why this is written out rather than
/// approximated.
bool skip_profile_tier_level(Bits& in, std::uint32_t sub_layers_minus1)
{
    // profile_space(2) tier(1) profile_idc(5) compatibility(32) four flags(4)
    // reserved(43) inbld(1) = 88 bits, then general_level_idc(8).
    if (!in.skip(88 + 8)) {
        return false;
    }
    std::vector<bool> profile_present(sub_layers_minus1);
    std::vector<bool> level_present(sub_layers_minus1);
    for (std::uint32_t i = 0; i < sub_layers_minus1; ++i) {
        std::uint32_t bit = 0;
        if (!in.u1(bit)) {
            return false;
        }
        profile_present[i] = bit != 0;
        if (!in.u1(bit)) {
            return false;
        }
        level_present[i] = bit != 0;
    }
    if (sub_layers_minus1 > 0 && !in.skip((8 - sub_layers_minus1) * 2)) {
        return false;
    }
    for (std::uint32_t i = 0; i < sub_layers_minus1; ++i) {
        if (profile_present[i] && !in.skip(88)) {
            return false;
        }
        if (level_present[i] && !in.skip(8)) {
            return false;
        }
    }
    return true;
}

/// `scaling_list_data`, likewise skipped exactly.
bool skip_scaling_list_data(Bits& in)
{
    for (std::uint32_t size_id = 0; size_id < 4; ++size_id) {
        for (std::uint32_t matrix_id = 0; matrix_id < 6;
             matrix_id += (size_id == 3) ? 3u : 1u) {
            std::uint32_t predicted = 0;
            if (!in.u1(predicted)) {
                return false;
            }
            if (predicted == 0) {
                std::uint32_t delta = 0;
                if (!in.ue(delta)) {
                    return false;
                }
                continue;
            }
            const std::uint32_t coefficients =
                std::min<std::uint32_t>(64u, 1u << (4u + (size_id << 1u)));
            std::int32_t ignored = 0;
            if (size_id > 1 && !se(in, ignored)) {
                return false;
            }
            for (std::uint32_t i = 0; i < coefficients; ++i) {
                if (!se(in, ignored)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// `st_ref_pic_set`, which is the one part that has to *remember* something.
///
/// A set may be coded as a difference from an earlier one, and the number of
/// bits that costs depends on how many pictures that earlier set had. So the
/// counts are kept as they are read; skipping them without would work on most
/// files and desynchronise on the ones that use prediction, which is again a
/// plausible wrong answer.
bool skip_st_ref_pic_set(Bits& in, std::uint32_t index, std::uint32_t count,
                         std::vector<std::uint32_t>& deltas)
{
    std::uint32_t predicted = 0;
    if (index != 0 && !in.u1(predicted)) {
        return false;
    }
    if (predicted != 0) {
        std::uint32_t delta_idx_minus1 = 0;
        if (index == count && !in.ue(delta_idx_minus1)) {
            return false;
        }
        std::uint32_t ignored = 0;
        std::int32_t ignored_signed = 0;
        (void)ignored_signed;
        if (!in.skip(1) || !in.ue(ignored)) { // delta_rps_sign, abs_delta_rps_minus1
            return false;
        }
        const std::uint32_t reference = index - (delta_idx_minus1 + 1u);
        if (reference >= deltas.size()) {
            return false;
        }
        std::uint32_t carried = 0;
        for (std::uint32_t j = 0; j <= deltas[reference]; ++j) {
            std::uint32_t used = 0;
            if (!in.u1(used)) {
                return false;
            }
            if (used == 0 && !in.skip(1)) { // use_delta_flag
                return false;
            }
            carried += used != 0 ? 1u : 0u;
        }
        deltas.push_back(carried);
        return true;
    }

    std::uint32_t negative = 0;
    std::uint32_t positive = 0;
    if (!in.ue(negative) || !in.ue(positive)) {
        return false;
    }
    // A corrupt file can ask for more than any level allows; refusing beats
    // spinning through a loop counted by whatever was in the bitstream.
    if (negative > 4096 || positive > 4096) {
        return false;
    }
    std::uint32_t ignored = 0;
    for (std::uint32_t i = 0; i < negative + positive; ++i) {
        if (!in.ue(ignored) || !in.skip(1)) {
            return false;
        }
    }
    deltas.push_back(negative + positive);
    return true;
}

} // namespace

HevcSize hevc_size(const AvcConfig& config)
{
    // The first fields of the SPS, walked exactly as `hevc_colour` walks them
    // and no further: pic_width_in_luma_samples and pic_height_in_luma_samples
    // come right after the chroma format.
    HevcSize out;
    for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
        if (nal.size() < 4 || ((nal[0] >> 1) & 0x3fu) != 33u) {
            continue;
        }
        Bits in{nal.data() + 2, nal.size() - 2};
        std::uint32_t sub_layers_minus1 = 0;
        if (!in.skip(4) || !in.un(3, sub_layers_minus1) || !in.skip(1)) {
            return out;
        }
        if (!skip_profile_tier_level(in, sub_layers_minus1)) {
            return out;
        }
        std::uint32_t value = 0;
        std::uint32_t chroma_format_idc = 0;
        if (!in.ue(value) || !in.ue(chroma_format_idc)) {
            return out;
        }
        if (chroma_format_idc == 3 && !in.skip(1)) {
            return out;
        }
        if (!in.ue(out.width) || !in.ue(out.height)) {
            return out;
        }
        out.valid = out.width != 0 && out.height != 0;
        out.visible_width = out.width;
        out.visible_height = out.height;
        // **The conformance window**, stated in units of the chroma
        // subsampling: SubWidthC and SubHeightC are 2 and 2 for 4:2:0, 2 and
        // 1 for 4:2:2, 1 and 1 for 4:4:4 and for monochrome (7.4.3.2.1).
        std::uint32_t window = 0;
        if (!in.u1(window) || window == 0) {
            return out;
        }
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        std::uint32_t top = 0;
        std::uint32_t bottom = 0;
        if (!in.ue(left) || !in.ue(right) || !in.ue(top) || !in.ue(bottom)) {
            return out;
        }
        const std::uint32_t sub_x = chroma_format_idc == 1 || chroma_format_idc == 2 ? 2u : 1u;
        const std::uint32_t sub_y = chroma_format_idc == 1 ? 2u : 1u;
        const std::uint64_t across = (static_cast<std::uint64_t>(left) + right) * sub_x;
        const std::uint64_t down = (static_cast<std::uint64_t>(top) + bottom) * sub_y;
        if (across >= out.width || down >= out.height) {
            return out; // a window that leaves no picture is a window nobody meant
        }
        out.crop_left = left * sub_x;
        out.crop_right = right * sub_x;
        out.crop_top = top * sub_y;
        out.crop_bottom = bottom * sub_y;
        out.visible_width = out.width - static_cast<std::uint32_t>(across);
        out.visible_height = out.height - static_cast<std::uint32_t>(down);
        return out;
    }
    return out;
}

HevcColour hevc_colour(const AvcConfig& config)
{
    HevcColour out;
    for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
        // NAL type 33 is the sequence parameter set. The header is two bytes.
        if (nal.size() < 4 || ((nal[0] >> 1) & 0x3fu) != 33u) {
            continue;
        }
        Bits in{nal.data() + 2, nal.size() - 2};

        std::uint32_t sub_layers_minus1 = 0;
        if (!in.skip(4) || !in.un(3, sub_layers_minus1) || !in.skip(1)) {
            return out;
        }
        if (!skip_profile_tier_level(in, sub_layers_minus1)) {
            return out;
        }

        std::uint32_t value = 0;
        std::uint32_t chroma_format_idc = 0;
        if (!in.ue(value) || !in.ue(chroma_format_idc)) { // sps_seq_parameter_set_id
            return out;
        }
        if (chroma_format_idc == 3 && !in.skip(1)) { // separate_colour_plane_flag
            return out;
        }
        if (!in.ue(value) || !in.ue(value)) { // pic width, pic height
            return out;
        }
        std::uint32_t conformance = 0;
        if (!in.u1(conformance)) {
            return out;
        }
        if (conformance != 0) {
            for (int i = 0; i < 4; ++i) {
                if (!in.ue(value)) {
                    return out;
                }
            }
        }
        if (!in.ue(value) || !in.ue(value) || !in.ue(value)) {
            return out; // bit depths, log2_max_pic_order_cnt_lsb_minus4
        }
        std::uint32_t ordering = 0;
        if (!in.u1(ordering)) {
            return out;
        }
        for (std::uint32_t i = (ordering != 0 ? 0u : sub_layers_minus1);
             i <= sub_layers_minus1; ++i) {
            if (!in.ue(value) || !in.ue(value) || !in.ue(value)) {
                return out;
            }
        }
        for (int i = 0; i < 6; ++i) {
            // The four log2 sizes and the two transform hierarchy depths.
            if (!in.ue(value)) {
                return out;
            }
        }
        std::uint32_t scaling = 0;
        if (!in.u1(scaling)) {
            return out;
        }
        if (scaling != 0) {
            std::uint32_t present = 0;
            if (!in.u1(present)) {
                return out;
            }
            if (present != 0 && !skip_scaling_list_data(in)) {
                return out;
            }
        }
        std::uint32_t pcm = 0;
        if (!in.skip(2) || !in.u1(pcm)) { // amp_enabled, sample_adaptive_offset
            return out;
        }
        if (pcm != 0) {
            if (!in.skip(8) || !in.ue(value) || !in.ue(value) || !in.skip(1)) {
                return out;
            }
        }
        std::uint32_t sets = 0;
        if (!in.ue(sets) || sets > 64) {
            return out;
        }
        std::vector<std::uint32_t> deltas;
        deltas.reserve(sets);
        for (std::uint32_t i = 0; i < sets; ++i) {
            if (!skip_st_ref_pic_set(in, i, sets, deltas)) {
                return out;
            }
        }
        std::uint32_t long_term = 0;
        if (!in.u1(long_term)) {
            return out;
        }
        if (long_term != 0) {
            std::uint32_t count = 0;
            if (!in.ue(count) || count > 64) {
                return out;
            }
            // Each is log2_max_pic_order_cnt_lsb bits and a flag; the width was
            // read above as a minus-4 and is needed here.
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!in.skip(1)) { // used_by_curr_pic_lt_sps_flag, after the poc
                    return out;
                }
            }
        }
        if (!in.skip(2)) { // temporal_mvp, strong_intra_smoothing
            return out;
        }
        std::uint32_t vui = 0;
        if (!in.u1(vui) || vui == 0) {
            return out; // no VUI: unspecified, and saying so is the answer
        }

        std::uint32_t aspect = 0;
        if (!in.u1(aspect)) {
            return out;
        }
        if (aspect != 0) {
            std::uint32_t idc = 0;
            if (!in.un(8, idc)) {
                return out;
            }
            if (idc == 255 && !in.skip(32)) {
                return out;
            }
        }
        std::uint32_t overscan = 0;
        if (!in.u1(overscan)) {
            return out;
        }
        if (overscan != 0 && !in.skip(1)) {
            return out;
        }
        std::uint32_t signal = 0;
        if (!in.u1(signal)) {
            return out;
        }
        if (signal != 0) {
            std::uint32_t full_range = 0;
            std::uint32_t described = 0;
            if (!in.skip(3) || !in.u1(full_range) || !in.u1(described)) {
                return out;
            }
            out.full_range = full_range != 0;
            if (described != 0) {
                if (!in.un(8, out.primaries) || !in.un(8, out.transfer) ||
                    !in.un(8, out.matrix)) {
                    return out;
                }
                out.valid = true;
            }
        }
        // **Where the chroma sits**, which follows the colour in the VUI
        // whether or not the colour was stated (E.2.1). Only the top field's
        // is kept: nothing here is interlaced, and the two are the same for a
        // progressive stream.
        std::uint32_t located = 0;
        if (!in.u1(located) || located == 0) {
            return out;
        }
        std::uint32_t top = 0;
        std::uint32_t bottom = 0;
        if (!in.ue(top) || !in.ue(bottom) || top > 5) {
            return out;
        }
        out.chroma_loc = static_cast<int>(top);
        return out;
    }
    return out;
}

HdrMetadata hevc_hdr_metadata_in_sample(const AvcConfig& config, const std::uint8_t* sample,
                                        std::size_t bytes)
{
    AvcConfig seis;
    seis.length_size = config.length_size;
    const std::uint32_t prefix = config.length_size != 0 ? config.length_size : 4u;
    std::size_t at = 0;
    while (sample != nullptr && at + prefix <= bytes) {
        std::size_t length = 0;
        for (std::uint32_t i = 0; i < prefix; ++i) {
            length = (length << 8) | sample[at + i];
        }
        at += prefix;
        if (length == 0 || length > bytes - at) {
            break; // a length past the end is a truncated sample, left alone
        }
        // 39 is a prefix SEI; the slices and the parameter sets are skipped
        // over by their lengths rather than read.
        if (((sample[at] >> 1) & 0x3fu) == 39u) {
            seis.parameter_sets.emplace_back(sample + at, sample + at + length);
        }
        at += length;
    }
    return hevc_hdr_metadata(seis);
}

HdrMetadata hevc_hdr_metadata(const AvcConfig& config)
{
    HdrMetadata out;
    for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
        if (nal.size() < 3) {
            continue;
        }
        // HEVC's NAL header is two bytes and the type is six bits of the first.
        // 39 is a prefix SEI; 40 is a suffix one and carries nothing wanted here.
        if (((nal[0] >> 1) & 0x3fu) != 39u) {
            continue;
        }
        const std::vector<std::uint8_t> body = unescaped(nal.data() + 2, nal.size() - 2);

        std::size_t at = 0;
        while (at + 1 < body.size()) {
            // **Both fields are 0xFF-extended**, which is the one thing about
            // SEI framing that catches people: a payload type of 137 is one
            // byte and a type of 300 is two 0xFF bytes and a 46.
            std::uint32_t type = 0;
            while (at < body.size() && body[at] == 0xff) {
                type += 255;
                ++at;
            }
            if (at >= body.size()) {
                break;
            }
            type += body[at++];

            std::uint32_t size = 0;
            while (at < body.size() && body[at] == 0xff) {
                size += 255;
                ++at;
            }
            if (at >= body.size()) {
                break;
            }
            size += body[at++];
            if (at + size > body.size()) {
                break; // a payload that runs past its NAL is a NAL to leave alone
            }

            if (type == 137u && size >= 24u) {
                // display_primaries are stated green, blue, red; the ABI wants
                // red, green, blue.
                static constexpr int k_from[3] = {2, 0, 1}; // ABI index -> SEI index
                for (int abi = 0; abi < 3; ++abi) {
                    const std::size_t base = at + static_cast<std::size_t>(k_from[abi]) * 4;
                    out.primaries_x[abi] = be16_at(body, base);
                    out.primaries_y[abi] = be16_at(body, base + 2);
                }
                out.white_x = be16_at(body, at + 12);
                out.white_y = be16_at(body, at + 14);
                out.max_luminance = be32_at(body, at + 16);
                out.min_luminance = be32_at(body, at + 20);
                out.has_mastering = out.white_x != 0 && out.white_y != 0;
            } else if (type == 144u && size >= 4u) {
                out.max_content_light_level = be16_at(body, at);
                out.max_frame_average_light_level = be16_at(body, at + 2);
                out.has_light_levels = true;
            }
            at += size;
        }
    }
    return out;
}

bool parameter_sets_annex_b(const AvcConfig& config, std::vector<std::uint8_t>& out)
{
    if (!config.valid) {
        return false;
    }
    out.clear();
    for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
        out.insert(out.end(), std::begin(k_start_code), std::end(k_start_code));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return true;
}

bool to_annex_b(const AvcConfig& config, const std::uint8_t* sample, std::size_t bytes,
                bool with_parameter_sets, std::vector<std::uint8_t>& out)
{
    if (!config.valid || sample == nullptr) {
        return false;
    }
    out.clear();

    if (with_parameter_sets && !parameter_sets_annex_b(config, out)) {
        return false;
    }

    std::size_t at = 0;
    while (at < bytes) {
        if (bytes - at < config.length_size) {
            return false; // a length that does not fit is a truncated sample
        }
        std::uint64_t length = 0;
        for (std::uint32_t i = 0; i < config.length_size; ++i) {
            length = (length << 8) | sample[at + i];
        }
        at += config.length_size;
        if (length > bytes - at) {
            return false; // a NAL that runs past the end
        }
        if (length == 0) {
            continue; // an empty NAL unit says nothing; emitting a bare start
                      // code for it would say something
        }
        out.insert(out.end(), std::begin(k_start_code), std::end(k_start_code));
        out.insert(out.end(), sample + at, sample + at + static_cast<std::size_t>(length));
        at += static_cast<std::size_t>(length);
    }
    return true;
}

} // namespace mp::mft

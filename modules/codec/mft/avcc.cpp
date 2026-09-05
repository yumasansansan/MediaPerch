// SPDX-License-Identifier: GPL-3.0-or-later
#include "avcc.hpp"

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
    [[nodiscard]] std::size_t left() const noexcept { return left_; }

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

bool to_annex_b(const AvcConfig& config, const std::uint8_t* sample, std::size_t bytes,
                bool with_parameter_sets, std::vector<std::uint8_t>& out)
{
    if (!config.valid || sample == nullptr) {
        return false;
    }
    out.clear();

    if (with_parameter_sets) {
        for (const std::vector<std::uint8_t>& nal : config.parameter_sets) {
            out.insert(out.end(), std::begin(k_start_code), std::end(k_start_code));
            out.insert(out.end(), nal.begin(), nal.end());
        }
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

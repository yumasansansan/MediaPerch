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

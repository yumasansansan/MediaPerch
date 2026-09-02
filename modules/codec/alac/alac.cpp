// SPDX-License-Identifier: GPL-3.0-or-later
//
// See alac.hpp for why this file exists rather than a link line.

#include "alac.hpp"

#include <cstring>

namespace mp::alac {
namespace {

// --------------------------------------------------------------- constants
// These are the format's, and the names are the reference's so that the two can
// be read side by side.
constexpr int k_qbshift = 9;
constexpr int k_qb = 1 << k_qbshift;
constexpr int k_mmulshift = 2;
constexpr int k_mdenshift = k_qbshift - k_mmulshift - 1; // 6
constexpr int k_moff = 1 << (k_mdenshift - 2);           // 16
constexpr int k_bitoff = 24;
constexpr unsigned k_max_prefix_16 = 9;
constexpr unsigned k_max_prefix_32 = 9;
constexpr unsigned k_max_datatype_bits_16 = 16;
constexpr std::uint32_t k_mean_clamp = 0xffff;

enum Element : unsigned {
    id_sce = 0,
    id_cpe = 1,
    id_cce = 2,
    id_lfe = 3,
    id_dse = 4,
    id_pce = 5,
    id_fil = 6,
    id_end = 7
};

/// Leading zeros, with lead(0) == 32 -- the reference's convention, and the one
/// the adaptive Golomb parameters are tuned around.
inline unsigned lead(std::uint32_t x) noexcept
{
    unsigned n = 0;
    for (std::uint32_t bit = 1u << 31; bit != 0; bit >>= 1) {
        if ((x & bit) != 0) {
            return n;
        }
        ++n;
    }
    return 32;
}

inline int lg3a(std::uint32_t x) noexcept
{
    return 31 - static_cast<int>(lead(x + 3));
}

inline std::int32_t sign_of(std::int32_t v) noexcept
{
    return (v >> 31) | static_cast<std::int32_t>(static_cast<std::uint32_t>(-v) >> 31);
}

/// Sign-extends the low `bits` of `v`. `bits` is always 1..32 here.
inline std::int32_t extend(std::int32_t v, unsigned bits) noexcept
{
    if (bits >= 32) {
        return v;
    }
    const unsigned shift = 32 - bits;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << shift) >> shift;
}

// ------------------------------------------------------------- bit reading
//
// The one place where this file differs from the reference in kind rather than
// in detail.
//
// The reference reads with `read32bit(in + (bitPos >> 3))`, four bytes at a
// time, from a pointer it does not bound. Near the end of a packet that reads
// past the buffer, and with a crafted packet it reads a long way past. Here a
// peek beyond the end returns zeroes instead -- which is not an error, because
// the last sample of a legitimate packet genuinely peeks past the end -- and it
// is *consuming* past the end that is checked, once, where the answer matters.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t bytes) noexcept
        : data_(data), bits_(bytes * 8)
    {
    }

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    void seek(std::size_t bit) noexcept { pos_ = bit; }
    [[nodiscard]] std::size_t size() const noexcept { return bits_; }
    [[nodiscard]] bool exhausted() const noexcept { return pos_ > bits_; }
    [[nodiscard]] bool has(std::size_t n) const noexcept
    {
        return pos_ <= bits_ && (bits_ - pos_) >= n;
    }

    /// 32 bits starting at `bit`, MSB first, zero-padded past the end.
    ///
    /// Padding rather than failing is deliberate: the last sample of a perfectly
    /// good packet peeks past its end, so a peek is not evidence of anything.
    /// What is checked, and checked where it means something, is consumption.
    [[nodiscard]] std::uint32_t peek32(std::size_t bit) const noexcept
    {
        const std::size_t bytes = bits_ >> 3;
        const std::size_t first = bit >> 3;
        const unsigned off = static_cast<unsigned>(bit & 7u);

        auto at = [&](std::size_t i) -> std::uint64_t {
            return i < bytes ? static_cast<std::uint64_t>(data_[i]) : 0u;
        };

        std::uint64_t window = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            window = (window << 8) | at(first + i);
        }
        // `window` holds bits [first*8, first*8+40); take 32 starting at `off`.
        return static_cast<std::uint32_t>((window >> (8 - off)) & 0xffffffffu);
    }

    /// Reads `n` bits (0..32) and advances. Past the end this yields zeroes and
    /// leaves `exhausted()` true.
    std::uint32_t read(unsigned n) noexcept
    {
        if (n == 0) {
            return 0;
        }
        const std::uint32_t v = peek32(pos_);
        pos_ += n;
        return n >= 32 ? v : (v >> (32 - n));
    }

    void advance(std::size_t n) noexcept { pos_ += n; }

    /// The Golomb readers over-advance by design and then step back one bit.
    void retreat(std::size_t n) noexcept { pos_ = pos_ >= n ? pos_ - n : 0; }

    void byte_align() noexcept { pos_ = (pos_ + 7u) & ~static_cast<std::size_t>(7u); }

private:
    const std::uint8_t* data_;
    std::size_t bits_;
    std::size_t pos_ = 0;
};

// ------------------------------------------------------- adaptive Golomb

struct AgParams {
    std::uint32_t mb0 = 0;
    std::uint32_t pb = 0;
    std::uint32_t kb = 0;
    std::uint32_t wb = 0;
};

/// `k` indexes a shift, so it has to be bounded before it is used as one.
///
/// The reference derives k from values the bitstream controls and then writes
/// `(1 << k) - 1` and `x >> (32 - k)` without checking it. Both are undefined
/// for k of 0 or 32 and neither is unreachable from a crafted file.
inline unsigned clamp_k(int k) noexcept
{
    if (k < 1) {
        return 1;
    }
    if (k > 31) {
        return 31;
    }
    return static_cast<unsigned>(k);
}

std::uint32_t dyn_get_32bit(BitReader& r, std::uint32_t m, unsigned k, unsigned maxbits) noexcept
{
    const std::uint32_t stream = r.peek32(r.pos());
    std::uint32_t result = lead(~stream);

    if (result >= k_max_prefix_32) {
        const std::size_t at = r.pos() + k_max_prefix_32;
        const std::uint32_t v = r.peek32(at);
        result = maxbits >= 32 ? v : (v >> (32 - maxbits));
        r.advance(k_max_prefix_32 + maxbits);
        return result;
    }

    r.advance(result + 1);
    if (k != 1) {
        const std::uint32_t v = (stream << (result + 1)) >> (32 - k);
        r.advance(k);
        r.retreat(1);
        result = result * m;
        if (v >= 2) {
            result += v - 1;
            r.advance(1);
        }
    }
    return result;
}

std::uint32_t dyn_get(BitReader& r, std::uint32_t m, unsigned k) noexcept
{
    const std::uint32_t stream = r.peek32(r.pos());
    std::uint32_t pre = lead(~stream);

    if (pre >= k_max_prefix_16) {
        pre = k_max_prefix_16;
        r.advance(pre);
        const std::uint32_t result = (stream << pre) >> (32 - k_max_datatype_bits_16);
        r.advance(k_max_datatype_bits_16);
        return result;
    }

    r.advance(pre + 1);
    const std::uint32_t v = (stream << (pre + 1)) >> (32 - k);
    r.advance(k);
    std::uint32_t result = pre * m + v - 1u;
    if (v < 2) {
        result -= (v - 1u);
        r.retreat(1);
    }
    return result;
}

/// Decodes `count` residuals into `out`. False on a malformed stream.
bool dyn_decomp(const AgParams& p, BitReader& r, std::int32_t* out, std::uint32_t count,
                unsigned maxbits) noexcept
{
    std::uint32_t mb = p.mb0;
    std::uint32_t zmode = 0;
    std::uint32_t c = 0;

    while (c < count) {
        if (r.pos() >= r.size()) {
            return false;
        }

        std::uint32_t m = mb >> k_qbshift;
        unsigned k = clamp_k(lg3a(m));
        if (k > p.kb) {
            k = static_cast<unsigned>(p.kb);
        }
        k = clamp_k(static_cast<int>(k));
        m = (k >= 32) ? 0xffffffffu : ((1u << k) - 1u);

        const std::uint32_t n = dyn_get_32bit(r, m, k, maxbits);

        const std::uint32_t ndecode = n + zmode;
        const std::int32_t multiplier = -static_cast<std::int32_t>(ndecode & 1u) | 1;
        out[c] = static_cast<std::int32_t>((ndecode + 1u) >> 1) * multiplier;
        ++c;

        mb = p.pb * (n + zmode) + mb - ((p.pb * mb) >> k_qbshift);
        if (n > k_mean_clamp) {
            mb = k_mean_clamp;
        }

        zmode = 0;
        if (((mb << k_mmulshift) < static_cast<std::uint32_t>(k_qb)) && (c < count)) {
            zmode = 1;
            const int raw = static_cast<int>(lead(mb)) - k_bitoff +
                            static_cast<int>((mb + k_moff) >> k_mdenshift);
            const unsigned kz = clamp_k(raw);
            const std::uint32_t mz = ((1u << kz) - 1u) & p.wb;
            const std::uint32_t run = dyn_get(r, mz, kz);

            // The reference checks this too, and it is the one place it does.
            if (run > count - c) {
                return false;
            }
            for (std::uint32_t j = 0; j < run; ++j) {
                out[c++] = 0;
            }
            if (run >= 65535) {
                zmode = 0;
            }
            mb = 0;
        }
    }

    return !r.exhausted();
}

// --------------------------------------------------------------- predictor

/// The inverse of the adaptive LPC. `coefs` is updated as it goes, which is the
/// adaptation; it is the caller's copy and is refilled from the stream per
/// element, so nothing persists between frames.
void unpc_block(const std::int32_t* in, std::int32_t* out, std::uint32_t num,
                std::int16_t* coefs, unsigned numactive, unsigned chanbits,
                unsigned denshift) noexcept
{
    if (num == 0) {
        return;
    }
    out[0] = in[0];

    if (numactive == 0) {
        if (num > 1 && in != out) {
            std::memcpy(&out[1], &in[1], (num - 1) * sizeof(std::int32_t));
        }
        return;
    }

    if (numactive == 31) {
        // The "in place" mode, run before the real predictor.
        std::int32_t prev = out[0];
        for (std::uint32_t j = 1; j < num; ++j) {
            const std::int32_t del = in[j] + prev;
            prev = extend(del, chanbits);
            out[j] = prev;
        }
        return;
    }

    // Warm-up: the first `numactive` samples are plain deltas.
    //
    // The reference writes out[1..numactive] without checking that the frame is
    // that long, so a short partial frame with a large coefficient count writes
    // past the end of the buffer. `numactive` is five bits from the stream and
    // `num` can be as small as one.
    const std::uint32_t warm = numactive + 1u < num ? numactive : (num > 0 ? num - 1 : 0);
    for (std::uint32_t j = 1; j <= warm; ++j) {
        const std::int32_t del = in[j] + out[j - 1];
        out[j] = extend(del, chanbits);
    }
    if (warm < numactive) {
        return; // nothing left for the predictor proper
    }

    // denshift is four bits from the stream, and the reference computes
    // `1 << (denshift - 1)` unconditionally at function entry -- undefined when
    // the stream says zero, which it is free to.
    const std::int32_t denhalf = denshift == 0 ? 0 : (1 << (denshift - 1));
    const std::uint32_t lim = numactive + 1u;

    for (std::uint32_t j = lim; j < num; ++j) {
        std::int32_t sum = 0;
        const std::int32_t* pout = out + j - 1;
        const std::int32_t top = out[j - lim];
        for (unsigned k = 0; k < numactive; ++k) {
            sum += coefs[k] * (pout[-static_cast<std::ptrdiff_t>(k)] - top);
        }

        std::int32_t del = in[j];
        std::int32_t del0 = del;
        const std::int32_t sg = sign_of(del);
        del += top + ((sum + denhalf) >> denshift);
        out[j] = extend(del, chanbits);

        if (sg > 0) {
            for (int k = static_cast<int>(numactive) - 1; k >= 0; --k) {
                const std::int32_t dd = top - pout[-k];
                const std::int32_t sgn = sign_of(dd);
                coefs[k] = static_cast<std::int16_t>(coefs[k] - sgn);
                del0 -= (static_cast<std::int32_t>(numactive) - k) * ((sgn * dd) >> denshift);
                if (del0 <= 0) {
                    break;
                }
            }
        } else if (sg < 0) {
            for (int k = static_cast<int>(numactive) - 1; k >= 0; --k) {
                const std::int32_t dd = top - pout[-k];
                const std::int32_t sgn = sign_of(dd);
                coefs[k] = static_cast<std::int16_t>(coefs[k] + sgn);
                del0 -= (static_cast<std::int32_t>(numactive) - k) * ((-sgn * dd) >> denshift);
                if (del0 >= 0) {
                    break;
                }
            }
        }
    }
}

} // namespace

// ----------------------------------------------------------------- config

bool parse_config(const std::uint8_t* cookie, std::size_t bytes, Config& out) noexcept
{
    if (cookie == nullptr || bytes < 24) {
        return false;
    }
    auto be32 = [cookie](std::size_t at) {
        return (static_cast<std::uint32_t>(cookie[at]) << 24) |
               (static_cast<std::uint32_t>(cookie[at + 1]) << 16) |
               (static_cast<std::uint32_t>(cookie[at + 2]) << 8) |
               static_cast<std::uint32_t>(cookie[at + 3]);
    };

    out.frame_length = be32(0);
    out.compatible_version = cookie[4];
    out.bit_depth = cookie[5];
    out.pb = cookie[6];
    out.mb = cookie[7];
    out.kb = cookie[8];
    out.channels = cookie[9];
    out.max_run = static_cast<std::uint16_t>((cookie[10] << 8) | cookie[11]);
    out.max_frame_bytes = be32(12);
    out.avg_bit_rate = be32(16);
    out.sample_rate = be32(20);

    if (out.compatible_version != 0) {
        return false;
    }
    if (out.bit_depth != 16 && out.bit_depth != 20 && out.bit_depth != 24 &&
        out.bit_depth != 32) {
        return false;
    }
    if (out.channels == 0 || out.channels > k_max_channels) {
        return false;
    }
    if (out.frame_length == 0 || out.frame_length > k_max_frame_length) {
        return false;
    }
    if (out.sample_rate == 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- layouts

const ChannelLayout& layout_for(unsigned channels) noexcept
{
    // ALAC's channel order, from Apple's own table, mapped onto WAVE slots.
    //
    // 1: C            2: L R          3: C L R
    // 4: C L R Cs     5: C L R Ls Rs  6: C L R Ls Rs LFE
    // 7: C L R Ls Rs Cs LFE           8: C Lc Rc L R Ls Rs LFE
    //
    // `from[slot]` is the ALAC channel that belongs in that WAVE slot, with the
    // slots in ascending SPEAKER_* bit order.
    static const ChannelLayout table[9] = {
        /* 0 */ {0u, {0, 0, 0, 0, 0, 0, 0, 0}},
        /* 1 */ {0u, {0, 0, 0, 0, 0, 0, 0, 0}},
        /* 2 */ {0u, {0, 1, 0, 0, 0, 0, 0, 0}},
        /* 3: FL FR FC              */ {0x1u | 0x2u | 0x4u, {1, 2, 0, 0, 0, 0, 0, 0}},
        /* 4: FL FR FC BC           */
        {0x1u | 0x2u | 0x4u | 0x100u, {1, 2, 0, 3, 0, 0, 0, 0}},
        /* 5: FL FR FC BL BR        */
        {0x1u | 0x2u | 0x4u | 0x10u | 0x20u, {1, 2, 0, 3, 4, 0, 0, 0}},
        /* 6: FL FR FC LFE BL BR    */
        {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u, {1, 2, 0, 5, 3, 4, 0, 0}},
        /* 7: FL FR FC LFE BC SL SR */
        {0x1u | 0x2u | 0x4u | 0x8u | 0x100u | 0x200u | 0x400u, {1, 2, 0, 6, 5, 3, 4, 0}},
        /* 8: FL FR FC LFE BL BR FLC FRC */
        {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u,
         {3, 4, 0, 7, 5, 6, 1, 2}},
    };
    return table[channels <= k_max_channels ? channels : 0];
}

// ---------------------------------------------------------------- decoder

bool Decoder::init(const Config& cfg) noexcept
{
    ready_ = false;
    error_ = "";
    if (cfg.channels == 0 || cfg.channels > k_max_channels) {
        error_ = "channel count out of range";
        return false;
    }
    if (cfg.frame_length == 0 || cfg.frame_length > k_max_frame_length) {
        error_ = "frame length out of range";
        return false;
    }
    if (cfg.bit_depth != 16 && cfg.bit_depth != 20 && cfg.bit_depth != 24 &&
        cfg.bit_depth != 32) {
        error_ = "unsupported bit depth";
        return false;
    }

    cfg_ = cfg;
    const std::size_t n = cfg.frame_length;
    predictor_.assign(n, 0);
    mix_u_.assign(n, 0);
    mix_v_.assign(n, 0);
    // Two per frame because a channel pair fills it interleaved. Zeroed, and
    // zeroed again below per frame: the reference leaves it holding the previous
    // frame's contents and then ORs it into the output when a crafted stream
    // sets mixres without setting bytesShifted.
    shift_.assign(n * 2, 0);
    ready_ = true;
    return true;
}

std::uint32_t Decoder::decode(const std::uint8_t* packet, std::size_t bytes,
                              std::int32_t* out) noexcept
{
    if (!ready_ || packet == nullptr || out == nullptr) {
        error_ = "decoder not initialised";
        return 0;
    }
    error_ = "";

    BitReader r{packet, bytes};
    const unsigned channels = cfg_.channels;
    const unsigned depth = cfg_.bit_depth;
    std::uint32_t samples = cfg_.frame_length;
    std::uint32_t produced = 0;
    unsigned channel_index = 0;

    std::int16_t coefs_u[32] = {};
    std::int16_t coefs_v[32] = {};

    while (channel_index < channels) {
        if (r.pos() >= r.size()) {
            error_ = "packet ended before the frame did";
            return 0;
        }

        const unsigned tag = r.read(3);
        if (tag == id_end) {
            break;
        }
        if (tag == id_cce || tag == id_pce) {
            error_ = "coupling and program-config elements are not part of ALAC";
            return 0;
        }
        if (tag == id_dse) {
            r.read(4);
            const bool align = r.read(1) != 0;
            std::uint32_t count = r.read(8);
            if (count == 255) {
                count += r.read(8);
            }
            if (align) {
                r.byte_align();
            }
            r.advance(static_cast<std::size_t>(count) * 8);
            if (r.exhausted()) {
                error_ = "data stream element runs past the packet";
                return 0;
            }
            continue;
        }
        if (tag == id_fil) {
            std::uint32_t count = r.read(4);
            if (count == 15) {
                count += r.read(8) - 1u;
            }
            r.advance(static_cast<std::size_t>(count) * 8);
            if (r.exhausted()) {
                error_ = "fill element runs past the packet";
                return 0;
            }
            continue;
        }

        const bool pair = (tag == id_cpe);
        const unsigned width = pair ? 2u : 1u;
        if (channel_index + width > channels) {
            error_ = "the frame describes more channels than the stream has";
            return 0;
        }

        r.read(4); // element instance tag
        if (r.read(12) != 0) {
            error_ = "reserved header bits are not zero";
            return 0;
        }

        const unsigned header = r.read(4);
        const bool partial = (header >> 3) != 0;
        unsigned bytes_shifted = (header >> 1) & 0x3u;
        const bool escape = (header & 1u) != 0;
        if (bytes_shifted == 3) {
            error_ = "invalid shift width";
            return 0;
        }
        // A channel pair codes one extra bit, because the mid/side residual
        // needs the range. The uncompressed path below overrides this for a
        // pair: there is no residual there, so there is no extra bit.
        unsigned chan_bits = depth - bytes_shifted * 8u + (pair ? 1u : 0u);

        if (partial) {
            const std::uint32_t hi = r.read(16);
            const std::uint32_t lo = r.read(16);
            samples = (hi << 16) | lo;
            // The buffers were sized from the config, and the config is the only
            // promise about frame length there is. The reference takes this
            // number and writes that many samples.
            if (samples == 0 || samples > cfg_.frame_length) {
                error_ = "partial frame claims more samples than the config allows";
                return 0;
            }
        }
        if (chan_bits == 0 || chan_bits > 32) {
            error_ = "channel bit width out of range";
            return 0;
        }

        std::uint8_t mix_bits = 0;
        std::int8_t mix_res = 0;
        std::size_t shift_start = 0;

        if (!escape) {
            mix_bits = static_cast<std::uint8_t>(r.read(8));
            mix_res = static_cast<std::int8_t>(r.read(8));

            unsigned mode_u = 0, den_u = 0, num_u = 0;
            unsigned mode_v = 0, den_v = 0, num_v = 0;
            std::uint32_t pb_u = 0, pb_v = 0;

            unsigned b = r.read(8);
            mode_u = b >> 4;
            den_u = b & 0xfu;
            b = r.read(8);
            pb_u = b >> 5;
            num_u = b & 0x1fu;
            for (unsigned i = 0; i < num_u; ++i) {
                coefs_u[i] = static_cast<std::int16_t>(r.read(16));
            }

            if (pair) {
                b = r.read(8);
                mode_v = b >> 4;
                den_v = b & 0xfu;
                b = r.read(8);
                pb_v = b >> 5;
                num_v = b & 0x1fu;
                for (unsigned i = 0; i < num_v; ++i) {
                    coefs_v[i] = static_cast<std::int16_t>(r.read(16));
                }
            }

            if (bytes_shifted != 0) {
                shift_start = r.pos();
                r.advance(static_cast<std::size_t>(bytes_shifted) * 8u * samples * width);
                if (r.exhausted()) {
                    error_ = "shift buffer runs past the packet";
                    return 0;
                }
            }

            AgParams p;
            p.mb0 = cfg_.mb;
            p.kb = cfg_.kb;
            p.wb = (cfg_.kb >= 32) ? 0xffffffffu : ((1u << cfg_.kb) - 1u);

            p.pb = (static_cast<std::uint32_t>(cfg_.pb) * pb_u) / 4u;
            if (!dyn_decomp(p, r, predictor_.data(), samples, chan_bits)) {
                error_ = "malformed residual stream";
                return 0;
            }
            if (mode_u == 0) {
                unpc_block(predictor_.data(), mix_u_.data(), samples, coefs_u, num_u, chan_bits,
                           den_u);
            } else {
                unpc_block(predictor_.data(), predictor_.data(), samples, nullptr, 31, chan_bits,
                           0);
                unpc_block(predictor_.data(), mix_u_.data(), samples, coefs_u, num_u, chan_bits,
                           den_u);
            }

            if (pair) {
                p.pb = (static_cast<std::uint32_t>(cfg_.pb) * pb_v) / 4u;
                if (!dyn_decomp(p, r, predictor_.data(), samples, chan_bits)) {
                    error_ = "malformed residual stream";
                    return 0;
                }
                if (mode_v == 0) {
                    unpc_block(predictor_.data(), mix_v_.data(), samples, coefs_v, num_v,
                               chan_bits, den_v);
                } else {
                    unpc_block(predictor_.data(), predictor_.data(), samples, nullptr, 31,
                               chan_bits, 0);
                    unpc_block(predictor_.data(), mix_v_.data(), samples, coefs_v, num_v,
                               chan_bits, den_v);
                }
            }
        } else {
            // Uncompressed: the samples are simply there, at chan_bits each.
            if (pair) {
                chan_bits = depth;
            }
            const std::size_t need =
                static_cast<std::size_t>(chan_bits) * samples * width;
            if (!r.has(need)) {
                error_ = "uncompressed frame runs past the packet";
                return 0;
            }
            for (std::uint32_t i = 0; i < samples; ++i) {
                mix_u_[i] = extend(static_cast<std::int32_t>(r.read(chan_bits)), chan_bits);
                if (pair) {
                    mix_v_[i] = extend(static_cast<std::int32_t>(r.read(chan_bits)), chan_bits);
                }
            }
            mix_bits = 0;
            mix_res = 0;
            bytes_shifted = 0;
        }

        // Always defined, never stale. When there is no shift the buffer is
        // zeroes, so the ORs below are the identity rather than whatever the
        // previous frame left behind.
        const std::size_t shift_words = static_cast<std::size_t>(samples) * width;
        std::memset(shift_.data(), 0, shift_words * sizeof(std::uint16_t));
        if (bytes_shifted != 0) {
            BitReader s{packet, bytes};
            s.seek(shift_start);
            const unsigned w = bytes_shifted * 8u;
            for (std::size_t i = 0; i < shift_words; ++i) {
                shift_[i] = static_cast<std::uint16_t>(s.read(w));
            }
        }

        const unsigned shift = bytes_shifted * 8u;
        std::int32_t* dst = out + channel_index;

        if (!pair) {
            for (std::uint32_t i = 0; i < samples; ++i) {
                const std::int32_t v =
                    (mix_u_[i] << shift) | static_cast<std::int32_t>(shift_[i]);
                dst[static_cast<std::size_t>(i) * channels] = v;
            }
        } else if (mix_res != 0) {
            for (std::uint32_t i = 0; i < samples; ++i) {
                const std::int32_t u = mix_u_[i];
                const std::int32_t v = mix_v_[i];
                const std::int32_t l = u + v - ((mix_res * v) >> mix_bits);
                const std::int32_t rr = l - v;
                std::int32_t* p = dst + static_cast<std::size_t>(i) * channels;
                p[0] = (l << shift) | static_cast<std::int32_t>(shift_[i * 2 + 0]);
                p[1] = (rr << shift) | static_cast<std::int32_t>(shift_[i * 2 + 1]);
            }
        } else {
            for (std::uint32_t i = 0; i < samples; ++i) {
                std::int32_t* p = dst + static_cast<std::size_t>(i) * channels;
                p[0] = (mix_u_[i] << shift) | static_cast<std::int32_t>(shift_[i * 2 + 0]);
                p[1] = (mix_v_[i] << shift) | static_cast<std::int32_t>(shift_[i * 2 + 1]);
            }
        }

        channel_index += width;
        produced = samples;
    }

    if (produced == 0) {
        error_ = "the frame produced no samples";
        return 0;
    }

    // A frame that stopped early leaves the rest silent rather than undefined.
    for (; channel_index < channels; ++channel_index) {
        for (std::uint32_t i = 0; i < produced; ++i) {
            out[static_cast<std::size_t>(i) * channels + channel_index] = 0;
        }
    }

    return produced;
}

} // namespace mp::alac

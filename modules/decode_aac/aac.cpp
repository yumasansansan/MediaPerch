// SPDX-License-Identifier: GPL-3.0-or-later
//
// See aac.hpp for why this file exists rather than a link line.

#include "aac.hpp"

#include "aac_tables.hpp"

#include <cmath>
#include <cstring>

namespace mp::aac {
namespace {

// ---------------------------------------------------------------- constants

// The four window sequences. Only EIGHT_SHORT changes how a frame is *parsed*,
// which is why the other three are unused until the filterbank exists -- they
// are named now so that the parser and the filterbank cannot end up using
// different numbers for the same thing.
[[maybe_unused]] constexpr unsigned k_only_long = 0;
[[maybe_unused]] constexpr unsigned k_long_start = 1;
constexpr unsigned k_eight_short = 2;
[[maybe_unused]] constexpr unsigned k_long_stop = 3;

constexpr unsigned k_id_sce = 0;
constexpr unsigned k_id_cpe = 1;
constexpr unsigned k_id_cce = 2;
constexpr unsigned k_id_lfe = 3;
constexpr unsigned k_id_dse = 4;
constexpr unsigned k_id_pce = 5;
constexpr unsigned k_id_fil = 6;
constexpr unsigned k_id_end = 7;

constexpr unsigned k_cb_zero = 0;
constexpr unsigned k_cb_esc = 11;
constexpr unsigned k_cb_noise = 13;
constexpr unsigned k_cb_intensity2 = 14;
constexpr unsigned k_cb_intensity = 15;

const std::uint32_t k_rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000,  7350};

/// Dimension, modulo and offset for each spectrum codebook.
///
/// A codebook index is a number in base `mod` with `dim` digits, each digit
/// minus `off` being one spectral value. That is the whole of the mapping --
/// 81 = 3^4, 64 = 8^2, 169 = 13^2, 289 = 17^2 -- so no table of tuples is
/// needed, only arithmetic. `unsigned_cb` says whether a sign bit follows each
/// non-zero value.
struct BookShape {
    std::uint8_t dim;
    std::uint8_t mod;
    std::int8_t off;
    bool unsigned_cb;
};

const BookShape k_books[12] = {
    {0, 0, 0, false},   // 0: ZERO, never decoded
    {4, 3, 1, false},   // 1
    {4, 3, 1, false},   // 2
    {4, 3, 0, true},    // 3
    {4, 3, 0, true},    // 4
    {2, 9, 4, false},   // 5
    {2, 9, 4, false},   // 6
    {2, 8, 0, true},    // 7
    {2, 8, 0, true},    // 8
    {2, 13, 0, true},   // 9
    {2, 13, 0, true},   // 10
    {2, 17, 0, true},   // 11: the escape codebook
};

// --------------------------------------------------------------- bit reader

/// MSB-first, and it cannot leave the packet.
///
/// Reading past the end yields zeroes and sets `overrun`, which every caller
/// checks once at the end of the frame rather than after every field. That is
/// deliberate: a syntax error in AAC usually shows up as a read that runs off
/// the end several fields later, so the useful question is "did this frame end
/// where its packet ends", not "did this particular field fit".
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t bytes) noexcept
        : data_(data), bits_(bytes * 8)
    {
    }

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size() const noexcept { return bits_; }
    [[nodiscard]] bool overrun() const noexcept { return pos_ > bits_; }
    [[nodiscard]] std::size_t left() const noexcept { return pos_ < bits_ ? bits_ - pos_ : 0; }

    std::uint32_t read(unsigned n) noexcept
    {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v = (v << 1) | bit();
        }
        return v;
    }

    unsigned bit() noexcept
    {
        if (pos_ >= bits_) {
            ++pos_;
            return 0;
        }
        const unsigned v = (data_[pos_ >> 3] >> (7u - (pos_ & 7u))) & 1u;
        ++pos_;
        return v;
    }

    void skip(std::size_t n) noexcept { pos_ += n; }

    void byte_align() noexcept { pos_ = (pos_ + 7u) & ~static_cast<std::size_t>(7u); }

private:
    const std::uint8_t* data_;
    std::size_t bits_;
    std::size_t pos_ = 0;
};

// -------------------------------------------------------------------- VLC
//
// A binary trie, because AAC's codebooks are not canonical.
//
// The standard lists each codebook in the order of the tuple it decodes to and
// assigns codewords freely; they are prefix-free but they are not consecutive
// within a length, so the usual canonical "shift a bit in and compare a range"
// decode does not apply. Two attempts at one failed against the real tables
// before this was checked rather than assumed.
//
// A trie makes no assumption at all: it is built from (length, codeword) and it
// fails to build if any codeword is a prefix of another, which is the only
// property the tables actually promise. Roughly six hundred nodes for the
// largest codebook, walked one bit at a time.
class Vlc {
public:
    /// `codes` is 32-bit because it has to be: the spectrum codebooks fit in
    /// sixteen, and the scalefactor codebook does not -- its longest codeword is
    /// nineteen bits and its largest value is 524287. Narrowing it to uint16
    /// silently destroys 56 of its 121 entries, which is exactly what the first
    /// attempt at this did.
    bool build(const std::uint32_t* codes, const std::uint8_t* bits, unsigned count) noexcept
    {
        used_ = 1;
        node_[0].child[0] = -1;
        node_[0].child[1] = -1;
        node_[0].symbol = -1;

        for (unsigned i = 0; i < count; ++i) {
            const unsigned len = bits[i];
            if (len == 0 || len > k_max_len) {
                return false;
            }
            int at = 0;
            for (unsigned b = 0; b < len; ++b) {
                const unsigned bit = (codes[i] >> (len - 1u - b)) & 1u;
                if (node_[at].symbol >= 0) {
                    return false; // a shorter codeword is a prefix of this one
                }
                if (node_[at].child[bit] < 0) {
                    if (used_ >= k_max_nodes) {
                        return false;
                    }
                    node_[used_].child[0] = -1;
                    node_[used_].child[1] = -1;
                    node_[used_].symbol = -1;
                    node_[at].child[bit] = static_cast<std::int16_t>(used_);
                    ++used_;
                }
                at = node_[at].child[bit];
            }
            if (node_[at].symbol >= 0 || node_[at].child[0] >= 0 || node_[at].child[1] >= 0) {
                return false; // duplicate, or this codeword is a prefix of another
            }
            node_[at].symbol = static_cast<std::int16_t>(i);
        }
        return true;
    }

    /// The symbol, or -1 if the bits do not spell one.
    [[nodiscard]] int decode(BitReader& r) const noexcept
    {
        int at = 0;
        for (unsigned depth = 0; depth <= k_max_len; ++depth) {
            if (node_[at].symbol >= 0) {
                return node_[at].symbol;
            }
            at = node_[at].child[r.bit()];
            if (at < 0) {
                return -1;
            }
        }
        return -1;
    }

private:
    static constexpr unsigned k_max_len = 24;
    static constexpr unsigned k_max_nodes = 1400;
    struct Node {
        std::int16_t child[2];
        std::int16_t symbol;
    };
    Node node_[k_max_nodes] = {};
    unsigned used_ = 0;
};

Vlc g_spectrum[12];
Vlc g_scalefactor;
bool g_tables_ready = false;

bool build_tables() noexcept
{
    if (g_tables_ready) {
        return true;
    }
    std::uint32_t wide[289];
    for (unsigned b = 1; b <= 11; ++b) {
        const unsigned n = k_spectral_sizes[b - 1];
        for (unsigned i = 0; i < n; ++i) {
            wide[i] = k_spectral_codes[b - 1][i];
        }
        if (!g_spectrum[b].build(wide, k_spectral_bits[b - 1], n)) {
            return false;
        }
    }
    if (!g_scalefactor.build(k_scalefactor_code, k_scalefactor_bits, 121)) {
        return false;
    }
    g_tables_ready = true;
    return true;
}

} // namespace

// ------------------------------------------------------------------ config

std::uint32_t rate_for_index(unsigned index) noexcept
{
    return index < 13 ? k_rates[index] : 0;
}

bool parse_asc(const std::uint8_t* asc, std::size_t bytes, Config& out) noexcept
{
    if (asc == nullptr || bytes < 2) {
        return false;
    }
    BitReader r{asc, bytes};

    unsigned object_type = r.read(5);
    if (object_type == 31) {
        object_type = 32 + r.read(6);
    }
    unsigned rate_index = r.read(4);
    std::uint32_t rate = 0;
    if (rate_index == 15) {
        rate = r.read(24);
    } else {
        rate = rate_for_index(rate_index);
    }
    const unsigned channel_config = r.read(4);

    // SBR and PS are signalled by wrapping AAC-LC in object type 5 or 29. This
    // decoder does not implement either, and saying so here rather than
    // producing the core at half the rate is the honest answer: the file goes
    // to decode_ffmpeg instead.
    if (object_type == 5 || object_type == 29) {
        return false;
    }
    if (object_type != 2) {
        return false;
    }
    // 0..7 are the classic configurations; 11..14 were added later and 12 is
    // 7.1. Anything else is not a layout this decoder has a mapping for.
    if (rate == 0 || channel_config == 8 || channel_config == 9 || channel_config == 10 ||
        channel_config > 14) {
        return false;
    }

    // GASpecificConfig
    const bool frame_960 = r.bit() != 0;
    if (r.bit() != 0) { // dependsOnCoreCoder
        return false;
    }
    if (r.bit() != 0) { // extensionFlag: not defined for AAC-LC
        return false;
    }
    if (r.overrun()) {
        return false;
    }

    out.object_type = object_type;
    out.rate_index = rate_index < 13 ? rate_index : 0;
    out.sample_rate = rate;
    out.channel_config = channel_config;
    out.frame_960 = frame_960;
    return true;
}

// ----------------------------------------------------------------- decoder

namespace {

struct Ics {
    unsigned window_sequence = 0;
    unsigned window_shape = 0;
    unsigned max_sfb = 0;
    unsigned num_windows = 1;
    unsigned num_window_groups = 1;
    unsigned group_len[k_windows] = {1};
    unsigned num_swb = 0;
    const std::uint16_t* swb_offset = nullptr;
    unsigned swb_count = 0; ///< entries in swb_offset, including the terminator
};

bool read_ics_info(BitReader& r, const Config& cfg, Ics& ics) noexcept
{
    r.bit(); // ics_reserved_bit
    ics.window_sequence = r.read(2);
    ics.window_shape = r.bit();

    if (ics.window_sequence == k_eight_short) {
        ics.max_sfb = r.read(4);
        ics.num_windows = k_windows;
        ics.num_window_groups = 1;
        ics.group_len[0] = 1;
        for (unsigned w = 1; w < k_windows; ++w) {
            if (r.bit() != 0) {
                ++ics.group_len[ics.num_window_groups - 1];
            } else {
                ics.group_len[ics.num_window_groups++] = 1;
            }
        }
        ics.num_swb = k_num_swb_128[cfg.rate_index];
        ics.swb_offset = k_swb_offset_128[cfg.rate_index];
    } else {
        ics.max_sfb = r.read(6);
        ics.num_windows = 1;
        ics.num_window_groups = 1;
        ics.group_len[0] = 1;
        if (r.bit() != 0) {
            // predictor_data_present. Main-profile prediction is not AAC-LC and
            // this decoder will not guess at it.
            return false;
        }
        ics.num_swb = k_num_swb_1024[cfg.rate_index];
        ics.swb_offset = k_swb_offset_1024[cfg.rate_index];
    }

    if (ics.swb_offset == nullptr || ics.max_sfb > ics.num_swb || ics.max_sfb > k_max_sfb) {
        return false;
    }
    return true;
}

/// Section data: which codebook covers which scalefactor bands.
bool read_section_data(BitReader& r, const Ics& ics, std::uint8_t sfb_cb[][k_max_sfb]) noexcept
{
    const unsigned bits = (ics.window_sequence == k_eight_short) ? 3u : 5u;
    const unsigned escape = (1u << bits) - 1u;

    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        unsigned k = 0;
        while (k < ics.max_sfb) {
            // Past the end every read returns zero, so a zero-length section
            // would spin here forever without this. The check belongs in the
            // loop that can fail to advance, not only in the one that reads.
            if (r.overrun()) {
                return false;
            }
            const unsigned cb = r.read(4);
            if (cb == 12) {
                return false; // reserved
            }
            unsigned len = 0;
            unsigned incr = r.read(bits);
            while (incr == escape) {
                len += escape;
                incr = r.read(bits);
                if (r.overrun()) {
                    return false;
                }
            }
            len += incr;
            if (k + len > ics.max_sfb) {
                return false;
            }
            for (unsigned i = 0; i < len; ++i) {
                sfb_cb[g][k++] = static_cast<std::uint8_t>(cb);
            }
        }
    }
    return true;
}

/// Scale factors, intensity positions and noise energies, all differential.
bool read_scale_factors(BitReader& r, const Ics& ics, const std::uint8_t sfb_cb[][k_max_sfb],
                        int global_gain, int sf[][k_max_sfb]) noexcept
{
    int scale = global_gain;
    int intensity = 0;
    int noise = global_gain - 90;
    bool noise_started = false;

    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        for (unsigned s = 0; s < ics.max_sfb; ++s) {
            const unsigned cb = sfb_cb[g][s];
            if (cb == k_cb_zero) {
                sf[g][s] = 0;
                continue;
            }
            if (cb == k_cb_intensity || cb == k_cb_intensity2) {
                const int delta = g_scalefactor.decode(r) - 60;
                if (delta < -60) {
                    return false;
                }
                intensity += delta;
                sf[g][s] = intensity;
                continue;
            }
            if (cb == k_cb_noise) {
                if (!noise_started) {
                    noise += static_cast<int>(r.read(9)) - 256;
                    noise_started = true;
                } else {
                    const int delta = g_scalefactor.decode(r) - 60;
                    if (delta < -60) {
                        return false;
                    }
                    noise += delta;
                }
                sf[g][s] = noise;
                continue;
            }
            const int delta = g_scalefactor.decode(r) - 60;
            if (delta < -60) {
                return false;
            }
            scale += delta;
            if (scale < 0 || scale > 255) {
                return false;
            }
            sf[g][s] = scale;
        }
    }
    return true;
}

bool read_pulse_data(BitReader& r, const Ics& ics, unsigned& pulse_start_sfb,
                     unsigned& count) noexcept
{
    count = r.read(2) + 1u;
    pulse_start_sfb = r.read(6);
    if (pulse_start_sfb >= ics.num_swb) {
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        r.read(5); // pulse_offset
        r.read(4); // pulse_amp
    }
    return true;
}

bool read_tns_data(BitReader& r, const Ics& ics) noexcept
{
    const bool is_short = ics.window_sequence == k_eight_short;
    const unsigned n_filt_bits = is_short ? 1u : 2u;
    const unsigned length_bits = is_short ? 4u : 6u;
    const unsigned order_bits = is_short ? 3u : 5u;
    const unsigned max_order = is_short ? 7u : 20u;

    for (unsigned w = 0; w < ics.num_windows; ++w) {
        const unsigned n_filt = r.read(n_filt_bits);
        unsigned coef_res = 0;
        if (n_filt != 0) {
            coef_res = r.bit();
        }
        for (unsigned f = 0; f < n_filt; ++f) {
            r.read(length_bits);
            const unsigned order = r.read(order_bits);
            if (order > max_order) {
                return false;
            }
            if (order != 0) {
                r.bit(); // direction
                const unsigned compress = r.bit();
                const unsigned coef_bits = coef_res + 3u - compress;
                for (unsigned i = 0; i < order; ++i) {
                    r.read(coef_bits);
                }
            }
        }
    }
    return true;
}

/// The spectrum itself, and the only place a codebook is used.
bool read_spectral_data(BitReader& r, const Ics& ics, const std::uint8_t sfb_cb[][k_max_sfb],
                        int quant[k_frame_len]) noexcept
{
    std::memset(quant, 0, sizeof(int) * k_frame_len);

    const unsigned win_len = (ics.window_sequence == k_eight_short) ? k_short_len : k_frame_len;
    unsigned group_start = 0;

    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        const unsigned group_windows = ics.group_len[g];
        for (unsigned s = 0; s < ics.max_sfb; ++s) {
            const unsigned cb = sfb_cb[g][s];
            if (cb == k_cb_zero || cb == k_cb_noise || cb == k_cb_intensity ||
                cb == k_cb_intensity2) {
                continue;
            }
            if (cb > 11) {
                return false;
            }
            const BookShape& shape = k_books[cb];
            const unsigned lo = ics.swb_offset[s];
            const unsigned hi = ics.swb_offset[s + 1];
            if (hi <= lo || hi > win_len) {
                return false;
            }

            for (unsigned w = 0; w < group_windows; ++w) {
                const unsigned base = (group_start + w) * win_len;
                for (unsigned k = lo; k < hi; k += shape.dim) {
                    const int index = g_spectrum[cb].decode(r);
                    if (index < 0) {
                        return false;
                    }
                    // The index is a base-`mod` number, most significant digit
                    // first; each digit less `off` is one spectral value.
                    int value[4] = {};
                    int rest = index;
                    for (int d = shape.dim - 1; d >= 0; --d) {
                        value[d] = (rest % shape.mod) - shape.off;
                        rest /= shape.mod;
                    }
                    // Signs for the whole tuple first, and only then the
                    // escapes. That order is the standard's and it is not
                    // interchangeable: reading sign, escape, sign, escape puts
                    // the escape's bits where the second sign bit should be, and
                    // the frame desynchronises from there. It shows up only in
                    // frames that contain an escape at all, which is why most
                    // packets parsed perfectly while this was wrong.
                    if (shape.unsigned_cb) {
                        for (unsigned d = 0; d < shape.dim; ++d) {
                            if (value[d] != 0 && r.bit() != 0) {
                                value[d] = -value[d];
                            }
                        }
                    }
                    if (cb == k_cb_esc) {
                        for (unsigned d = 0; d < shape.dim; ++d) {
                            if (value[d] != 16 && value[d] != -16) {
                                continue;
                            }
                            unsigned n = 4;
                            while (r.bit() != 0) {
                                ++n;
                                if (n > 20) {
                                    return false;
                                }
                            }
                            const std::uint32_t extra = r.read(n);
                            const int magnitude = static_cast<int>((1u << n) | extra);
                            value[d] = value[d] < 0 ? -magnitude : magnitude;
                        }
                    }
                    for (unsigned d = 0; d < shape.dim; ++d) {
                        if (base + k + d < k_frame_len) {
                            quant[base + k + d] = value[d];
                        }
                    }
                    if (r.overrun()) {
                        return false;
                    }
                }
            }
        }
        group_start += group_windows;
    }
    return true;
}

/// x = sign(q) * |q|^(4/3) * 2^((sf - 100) / 4)
void dequantise(const Ics& ics, const std::uint8_t sfb_cb[][k_max_sfb],
                const int sf[][k_max_sfb], const int quant[k_frame_len],
                float out[k_frame_len]) noexcept
{
    std::memset(out, 0, sizeof(float) * k_frame_len);
    const unsigned win_len = (ics.window_sequence == k_eight_short) ? k_short_len : k_frame_len;
    unsigned group_start = 0;

    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        for (unsigned s = 0; s < ics.max_sfb; ++s) {
            const unsigned cb = sfb_cb[g][s];
            if (cb == k_cb_zero || cb == k_cb_noise || cb == k_cb_intensity ||
                cb == k_cb_intensity2 || cb > 11) {
                continue;
            }
            const float gain = std::pow(2.0F, static_cast<float>(sf[g][s] - 100) * 0.25F);
            const unsigned lo = ics.swb_offset[s];
            const unsigned hi = ics.swb_offset[s + 1];
            for (unsigned w = 0; w < ics.group_len[g]; ++w) {
                const unsigned base = (group_start + w) * win_len;
                for (unsigned k = lo; k < hi && base + k < k_frame_len; ++k) {
                    const int q = quant[base + k];
                    if (q == 0) {
                        continue;
                    }
                    const float magnitude =
                        std::pow(static_cast<float>(q < 0 ? -q : q), 4.0F / 3.0F);
                    out[base + k] = (q < 0 ? -magnitude : magnitude) * gain;
                }
            }
        }
        group_start += ics.group_len[g];
    }
}

} // namespace

bool Decoder::init(const Config& cfg) noexcept
{
    ready_ = false;
    error_ = "";
    if (!build_tables()) {
        error_ = "a Huffman codebook is not a prefix code";
        return false;
    }
    if (cfg.object_type != 2 || cfg.sample_rate == 0 || cfg.rate_index >= 13) {
        error_ = "not an AAC-LC configuration";
        return false;
    }
    if (cfg.frame_960) {
        error_ = "960-sample frames are not implemented";
        return false;
    }
    cfg_ = cfg;
    ready_ = true;
    return true;
}

bool Decoder::decode_frame(const std::uint8_t* packet, std::size_t bytes) noexcept
{
    if (!ready_ || packet == nullptr) {
        error_ = "decoder not initialised";
        return false;
    }
    error_ = "";
    channels_ = 0;

    BitReader r{packet, bytes};
    bits_given_ = r.size();

    static std::uint8_t sfb_cb[k_windows][k_max_sfb];
    static int sf[k_windows][k_max_sfb];
    static int quant[k_frame_len];

    auto read_ics = [&](Ics& ics, Channel& out, bool have_common, const Ics& common) -> bool {
        const unsigned global_gain = r.read(8);
        if (!have_common) {
            if (!read_ics_info(r, cfg_, ics)) {
                error_ = "individual_channel_stream: bad ics_info";
                return false;
            }
        } else {
            ics = common;
        }
        std::memset(sfb_cb, 0, sizeof(sfb_cb));
        if (!read_section_data(r, ics, sfb_cb)) {
            error_ = "individual_channel_stream: bad section_data";
            return false;
        }
        if (!read_scale_factors(r, ics, sfb_cb, static_cast<int>(global_gain), sf)) {
            error_ = "individual_channel_stream: bad scale_factor_data";
            return false;
        }
        if (r.bit() != 0) { // pulse_data_present
            unsigned start = 0;
            unsigned count = 0;
            if (ics.window_sequence == k_eight_short || !read_pulse_data(r, ics, start, count)) {
                error_ = "individual_channel_stream: bad pulse_data";
                return false;
            }
        }
        if (r.bit() != 0) { // tns_data_present
            if (!read_tns_data(r, ics)) {
                error_ = "individual_channel_stream: bad tns_data";
                return false;
            }
        }
        if (r.bit() != 0) { // gain_control_data_present: SSR only
            error_ = "gain control is not part of AAC-LC";
            return false;
        }
        if (!read_spectral_data(r, ics, sfb_cb, quant)) {
            error_ = "individual_channel_stream: bad spectral_data";
            return false;
        }

        out.window_sequence = ics.window_sequence;
        out.window_shape = ics.window_shape;
        out.max_sfb = ics.max_sfb;
        out.num_window_groups = ics.num_window_groups;
        for (unsigned g = 0; g < k_windows; ++g) {
            out.group_len[g] = ics.group_len[g];
        }
        dequantise(ics, sfb_cb, sf, quant, out.coeffs);
        return true;
    };

    while (true) {
        if (r.left() < 3) {
            error_ = "the packet ended before the frame did";
            return false;
        }
        const unsigned id = r.read(3);
        if (id == k_id_end) {
            break;
        }

        switch (id) {
        case k_id_sce:
        case k_id_lfe: {
            if (channels_ >= k_max_channels) {
                error_ = "more channels than this decoder holds";
                return false;
            }
            r.read(4); // element_instance_tag
            Ics ics;
            if (!read_ics(ics, chan_[channels_], false, ics)) {
                return false;
            }
            ++channels_;
            break;
        }
        case k_id_cpe: {
            if (channels_ + 2 > k_max_channels) {
                error_ = "more channels than this decoder holds";
                return false;
            }
            r.read(4); // element_instance_tag
            Ics common;
            const bool common_window = r.bit() != 0;
            if (common_window) {
                if (!read_ics_info(r, cfg_, common)) {
                    error_ = "channel_pair_element: bad ics_info";
                    return false;
                }
                const unsigned ms_mask_present = r.read(2);
                if (ms_mask_present == 3) {
                    error_ = "channel_pair_element: reserved ms_mask_present";
                    return false;
                }
                if (ms_mask_present == 1) {
                    for (unsigned g = 0; g < common.num_window_groups; ++g) {
                        for (unsigned s = 0; s < common.max_sfb; ++s) {
                            r.bit();
                        }
                    }
                }
            }
            Ics left;
            Ics right;
            if (!read_ics(left, chan_[channels_], common_window, common)) {
                return false;
            }
            if (!read_ics(right, chan_[channels_ + 1], common_window, common)) {
                return false;
            }
            channels_ += 2;
            break;
        }
        case k_id_dse: {
            r.read(4); // element_instance_tag
            const bool align = r.bit() != 0;
            std::uint32_t count = r.read(8);
            if (count == 255) {
                count += r.read(8);
            }
            if (align) {
                r.byte_align();
            }
            r.skip(static_cast<std::size_t>(count) * 8);
            break;
        }
        case k_id_fil: {
            std::uint32_t count = r.read(4);
            if (count == 15) {
                count += r.read(8) - 1u;
            }
            r.skip(static_cast<std::size_t>(count) * 8);
            break;
        }
        case k_id_pce: {
            // Parsed only so the bit count stays honest; the channel map it
            // carries is not used, because a file that needs one is a file this
            // decoder has no layout for anyway.
            r.read(4);           // element_instance_tag
            r.read(2);           // object_type
            r.read(4);           // sampling_frequency_index
            const unsigned front = r.read(4);
            const unsigned side = r.read(4);
            const unsigned back = r.read(4);
            const unsigned lfe = r.read(2);
            const unsigned assoc = r.read(3);
            const unsigned cc = r.read(4);
            if (r.bit() != 0) {
                r.read(4);
            }
            if (r.bit() != 0) {
                r.read(4);
            }
            if (r.bit() != 0) {
                r.read(3);
            }
            for (unsigned i = 0; i < front + side + back; ++i) {
                r.read(5); // element_is_cpe + element_tag_select
            }
            for (unsigned i = 0; i < lfe + assoc; ++i) {
                r.read(4);
            }
            for (unsigned i = 0; i < cc; ++i) {
                r.read(5);
            }
            r.byte_align();
            const std::uint32_t comment = r.read(8);
            r.skip(static_cast<std::size_t>(comment) * 8);
            break;
        }
        case k_id_cce:
        default:
            error_ = "coupling channel elements are not implemented";
            return false;
        }

        if (r.overrun()) {
            error_ = "an element ran past the end of the packet";
            return false;
        }
    }

    r.byte_align();
    bits_used_ = r.pos();

    if (r.overrun()) {
        error_ = "the frame ran past the end of the packet";
        return false;
    }
    if (channels_ == 0) {
        error_ = "the frame carried no channels";
        return false;
    }
    return true;
}

} // namespace mp::aac

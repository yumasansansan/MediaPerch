// SPDX-License-Identifier: GPL-3.0-or-later
//
// See aac.hpp for why this file exists rather than a link line.

#include "aac.hpp"

#include "aac_tables.hpp"

#include <cmath>
#include <new>
#include <numbers>
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

// ------------------------------------------------------------- the filterbank

/// The zeroth-order modified Bessel function, for the KBD window.
double bessel_i0(double x) noexcept
{
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i < 50; ++i) {
        term *= (x * x) / (4.0 * static_cast<double>(i) * static_cast<double>(i));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

/// Sine and Kaiser-Bessel-derived windows, long and short.
///
/// Both are computed rather than tabulated: the sine window is a sine, and KBD
/// is a running sum of a Kaiser window normalised and square-rooted. Tabulating
/// them would add two thousand more constants to check.
struct Windows {
    float sine_long[k_frame_len] = {};
    float sine_short[k_short_len] = {};
    float kbd_long[k_frame_len] = {};
    float kbd_short[k_short_len] = {};
};

Windows g_win;
bool g_win_ready = false;

void build_kbd(float* out, unsigned n, double alpha) noexcept
{
    // w[j] = I0(pi*alpha*sqrt(1 - (2j/n - 1)^2)), then the running sum of those,
    // normalised by the total and square-rooted.
    double kaiser[k_frame_len + 1];
    double total = 0.0;
    for (unsigned j = 0; j <= n; ++j) {
        const double x = 2.0 * static_cast<double>(j) / static_cast<double>(n) - 1.0;
        kaiser[j] = bessel_i0(std::numbers::pi * alpha * std::sqrt(1.0 - x * x));
        total += kaiser[j];
    }
    double running = 0.0;
    for (unsigned i = 0; i < n; ++i) {
        running += kaiser[i];
        out[i] = static_cast<float>(std::sqrt(running / total));
    }
}

void build_windows() noexcept
{
    if (g_win_ready) {
        return;
    }
    // W_SIN_LEFT,N(i) = sin(pi/N (i + 1/2)) for 0 <= i < N/2, with N the whole
    // window length -- so a quarter sine rising from ~0 to 1 across the half,
    // not a half sine peaking in the middle. The difference is invisible until
    // it is checked against Princen-Bradley: the right one satisfies
    // w[i]^2 + w[N/2-1-i]^2 == 1 and the wrong one does not, which is exactly
    // the condition the overlap relies on to cancel.
    for (unsigned i = 0; i < k_frame_len; ++i) {
        g_win.sine_long[i] = static_cast<float>(
            std::sin(std::numbers::pi / (2.0 * k_frame_len) * (i + 0.5)));
    }
    for (unsigned i = 0; i < k_short_len; ++i) {
        g_win.sine_short[i] = static_cast<float>(
            std::sin(std::numbers::pi / (2.0 * k_short_len) * (i + 0.5)));
    }
    build_kbd(g_win.kbd_long, k_frame_len, 4.0);
    build_kbd(g_win.kbd_short, k_short_len, 6.0);
    g_win_ready = true;
}

const float* window_for(unsigned shape, bool is_short) noexcept
{
    if (is_short) {
        return shape != 0 ? g_win.kbd_short : g_win.sine_short;
    }
    return shape != 0 ? g_win.kbd_long : g_win.sine_long;
}

/// The inverse MDCT, straight from the definition.
///
/// N output samples from N/2 coefficients, at O(N^2). That is slow -- two
/// million multiply-adds per long window -- and it is deliberate for now: the
/// definition is checkable by eye against the standard, and an FFT-based version
/// can be swapped in once there is something correct to check it against.
void imdct(const float* spec, float* out, unsigned n) noexcept
{
    const unsigned half = n / 2;
    const double n0 = (static_cast<double>(half) + 1.0) / 2.0;
    const double scale = 2.0 / static_cast<double>(n);
    for (unsigned i = 0; i < n; ++i) {
        double acc = 0.0;
        const double a = 2.0 * std::numbers::pi / static_cast<double>(n) *
                         (static_cast<double>(i) + n0);
        for (unsigned k = 0; k < half; ++k) {
            acc += static_cast<double>(spec[k]) * std::cos(a * (static_cast<double>(k) + 0.5));
        }
        out[i] = static_cast<float>(acc * scale);
    }
}

} // namespace

std::uint32_t rate_for_index(unsigned index) noexcept
{
    return index < 13 ? k_rates[index] : 0;
}

const ChannelLayout& layout_for_config(unsigned channel_config) noexcept
{
    // Decoded order per configuration, from ISO/IEC 14496-3 Table 1.19:
    //   1  C                            2  L R
    //   3  C L R                        4  C L R Cs
    //   5  C L R Ls Rs                  6  C L R Ls Rs LFE
    //   7  C L R Ls Rs Lr Rr LFE       11  C L R Ls Rs Cs LFE
    //  12  C L R Ls Rs Lr Rr LFE
    //
    // `from[slot]` is the decoded channel that belongs in that WAVE slot, with
    // the slots in ascending SPEAKER_* bit order.
    static const ChannelLayout table[15] = {
        /* 0  */ {0u, 0, {0}},
        /* 1  */ {0u, 1, {0}},
        /* 2  */ {0u, 2, {0, 1}},
        /* 3  */ {0x1u | 0x2u | 0x4u, 3, {1, 2, 0}},
        /* 4  */ {0x1u | 0x2u | 0x4u | 0x100u, 4, {1, 2, 0, 3}},
        /* 5  */ {0x1u | 0x2u | 0x4u | 0x10u | 0x20u, 5, {1, 2, 0, 3, 4}},
        /* 6  */ {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u, 6, {1, 2, 0, 5, 3, 4}},
        /* 7  */ {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x200u | 0x400u, 8,
                  {1, 2, 0, 7, 5, 6, 3, 4}},
        /* 8  */ {0u, 0, {0}},
        /* 9  */ {0u, 0, {0}},
        /* 10 */ {0u, 0, {0}},
        /* 11 */ {0x1u | 0x2u | 0x4u | 0x8u | 0x100u | 0x200u | 0x400u, 7,
                  {1, 2, 0, 6, 5, 3, 4}},
        /* 12 */ {0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x200u | 0x400u, 8,
                  {1, 2, 0, 7, 5, 6, 3, 4}},
        /* 13 */ {0u, 0, {0}},
        /* 14 */ {0u, 0, {0}},
    };
    return table[channel_config < 15 ? channel_config : 0];
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

/// One TNS filter, as read and then as coefficients.
struct TnsFilter {
    unsigned order = 0;
    unsigned length = 0;
    unsigned direction = 0;
    float lpc[k_max_tns_order + 1] = {};
};

struct Tns {
    bool present = false;
    unsigned n_filt[k_windows] = {};
    TnsFilter filt[k_windows][4];
};

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

/// Quantised reflection coefficients to LPC, by the standard's recursion.
///
/// The reflection values are not a table: the standard defines them as
/// `sin(q / iqfac)`, with a slightly different scale for negative q, so they are
/// computed. That is worth saying because it is the one place in AAC where a
/// table would have been expected and is not needed.
void tns_coefficients(const int* quant, unsigned order, unsigned coef_res,
                      unsigned coef_compress, float* lpc) noexcept
{
    const unsigned bits = coef_res + 3u - coef_compress;
    const double half = static_cast<double>(1u << (bits - 1u));
    const double iqfac = (half - 0.5) / (std::numbers::pi / 2.0);
    const double iqfac_m = (half + 0.5) / (std::numbers::pi / 2.0);

    double parcor[k_max_tns_order] = {};
    for (unsigned i = 0; i < order; ++i) {
        const double q = quant[i];
        parcor[i] = std::sin(q / (q >= 0.0 ? iqfac : iqfac_m));
    }

    double a[k_max_tns_order + 1] = {};
    double b[k_max_tns_order + 1] = {};
    for (unsigned m = 1; m <= order; ++m) {
        for (unsigned i = 1; i < m; ++i) {
            b[i] = a[i] + parcor[m - 1] * a[m - i];
        }
        for (unsigned i = 1; i < m; ++i) {
            a[i] = b[i];
        }
        a[m] = parcor[m - 1];
    }
    for (unsigned i = 0; i <= order; ++i) {
        lpc[i] = static_cast<float>(a[i]);
    }
}

bool read_tns_data(BitReader& r, const Ics& ics, Tns& tns) noexcept
{
    const bool is_short = ics.window_sequence == k_eight_short;
    const unsigned n_filt_bits = is_short ? 1u : 2u;
    const unsigned length_bits = is_short ? 4u : 6u;
    const unsigned order_bits = is_short ? 3u : 5u;
    const unsigned max_order = is_short ? 7u : 20u;

    tns.present = true;
    for (unsigned w = 0; w < ics.num_windows; ++w) {
        const unsigned n_filt = r.read(n_filt_bits);
        tns.n_filt[w] = n_filt > 4 ? 4 : n_filt;
        unsigned coef_res = 0;
        if (n_filt != 0) {
            coef_res = r.bit();
        }
        for (unsigned f = 0; f < n_filt; ++f) {
            const unsigned length = r.read(length_bits);
            const unsigned order = r.read(order_bits);
            if (order > max_order) {
                return false;
            }
            unsigned direction = 0;
            int quant[k_max_tns_order] = {};
            unsigned compress = 0;
            if (order != 0) {
                direction = r.bit();
                compress = r.bit();
                const unsigned coef_bits = coef_res + 3u - compress;
                for (unsigned i = 0; i < order; ++i) {
                    const std::uint32_t raw = r.read(coef_bits);
                    // Signed, in `coef_bits` bits.
                    const std::int32_t sign = 1 << (coef_bits - 1);
                    quant[i] = static_cast<int>(raw) -
                               ((static_cast<std::int32_t>(raw) & sign) != 0 ? (sign << 1) : 0);
                }
            }
            if (f < 4) {
                TnsFilter& out = tns.filt[w][f];
                out.order = order;
                out.length = length;
                out.direction = direction;
                if (order != 0) {
                    tns_coefficients(quant, order, coef_res, compress, out.lpc);
                }
            }
        }
    }
    return true;
}

/// The inverse of the encoder's temporal noise shaping: an all-pole filter run
/// across frequency rather than time.
void apply_tns(const Ics& ics, const Tns& tns, unsigned rate_index, float* coeffs) noexcept
{
    if (!tns.present) {
        return;
    }
    const bool is_short = ics.window_sequence == k_eight_short;
    const unsigned max_bands = is_short ? k_tns_max_bands_128[rate_index]
                                        : k_tns_max_bands_1024[rate_index];
    const unsigned win_len = is_short ? k_short_len : k_frame_len;

    for (unsigned w = 0; w < ics.num_windows; ++w) {
        unsigned bottom = ics.num_swb;
        for (unsigned f = 0; f < tns.n_filt[w]; ++f) {
            const TnsFilter& filt = tns.filt[w][f];
            const unsigned top = bottom;
            bottom = filt.length < top ? top - filt.length : 0;
            if (filt.order == 0) {
                continue;
            }
            const unsigned lo_band = bottom < max_bands ? bottom : max_bands;
            const unsigned hi_band = top < max_bands ? top : max_bands;
            if (lo_band > ics.num_swb || hi_band > ics.num_swb) {
                continue;
            }
            int start = static_cast<int>(ics.swb_offset[lo_band]);
            const int end = static_cast<int>(ics.swb_offset[hi_band]);
            const int size = end - start;
            if (size <= 0) {
                continue;
            }
            int inc = 1;
            if (filt.direction != 0) {
                inc = -1;
                start = end - 1;
            }
            start += static_cast<int>(w * win_len);

            for (int m = 0; m < size; ++m) {
                const unsigned taps =
                    static_cast<unsigned>(m) < filt.order ? static_cast<unsigned>(m) : filt.order;
                float acc = coeffs[start];
                for (unsigned i = 1; i <= taps; ++i) {
                    acc -= coeffs[start - static_cast<int>(i) * inc] * filt.lpc[i];
                }
                coeffs[start] = acc;
                start += inc;
            }
        }
    }
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

/// Perceptual noise substitution: a band the encoder replaced with a level.
///
/// Codebook 13 says "there was noise here, this loud" and sends no coefficients
/// at all. A decoder that skips those bands -- as this one did at first -- emits
/// silence across them, which on the opening frame of a noise signal was 22 of
/// 49 bands and showed up as output eight times too quiet and uncorrelated with
/// anything.
///
/// **The samples in such a band are arbitrary by design.** The standard fixes
/// the band's energy and says nothing about which noise fills it, so two
/// conformant decoders differ there and no comparison between them can be
/// sample-exact. The generator here is the one FFmpeg uses, seed included, for
/// exactly one reason: it makes the rest of this decoder checkable against
/// FFmpeg to the last bit. It is a testing convenience and not a claim about
/// the format.
std::uint32_t lcg_next(std::uint32_t state) noexcept
{
    return state * 1664525u + 1013904223u;
}

void apply_pns(const Ics& ics, const std::uint8_t sfb_cb[][k_max_sfb],
               const int sf[][k_max_sfb], std::uint32_t& rng, float* out) noexcept
{
    const unsigned win_len = (ics.window_sequence == k_eight_short) ? k_short_len : k_frame_len;
    unsigned group_start = 0;

    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        for (unsigned sb = 0; sb < ics.max_sfb; ++sb) {
            if (sfb_cb[g][sb] != k_cb_noise) {
                continue;
            }
            const unsigned lo = ics.swb_offset[sb];
            const unsigned hi = ics.swb_offset[sb + 1];
            // Not the same exponent as a normal band. A normal scalefactor is
            // a gain, 2^((sf-100)/4); a noise scalefactor names the band's total
            // *energy*, 2^(sf/2), so the amplitude the band is normalised to is
            // its square root.
            const float gain = std::pow(2.0F, static_cast<float>(sf[g][sb]) * 0.25F);
            for (unsigned w = 0; w < ics.group_len[g]; ++w) {
                const unsigned base = (group_start + w) * win_len;
                double energy = 0.0;
                for (unsigned k = lo; k < hi && base + k < k_frame_len; ++k) {
                    rng = lcg_next(rng);
                    const float v = static_cast<float>(static_cast<std::int32_t>(rng));
                    out[base + k] = v;
                    energy += static_cast<double>(v) * v;
                }
                if (energy <= 0.0) {
                    continue;
                }
                const float scale = static_cast<float>(gain / std::sqrt(energy));
                for (unsigned k = lo; k < hi && base + k < k_frame_len; ++k) {
                    out[base + k] *= scale;
                }
            }
        }
        group_start += ics.group_len[g];
    }
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

/// Per-channel working state, one frame's worth.
struct State {
    struct Chan {
        Ics ics;
        Tns tns;
        std::uint8_t sfb_cb[k_windows][k_max_sfb] = {};
        int sf[k_windows][k_max_sfb] = {};
        float coeffs[k_frame_len] = {};
    };
    Chan chan[k_max_channels];
    int quant[k_frame_len] = {};
    /// For a channel pair: the second channel of the pair, and whether M/S is on
    /// for each band. `pair_of[i]` is the other channel, or -1.
    int pair_of[k_max_channels] = {};
    bool ms_used[k_max_channels][k_windows][k_max_sfb] = {};
};

namespace {

// ------------------------------------------------------------- joint stereo

/// Mid/side and intensity, both of which turn one channel's spectrum into two.
///
/// They are disjoint by construction: intensity is signalled by the *right*
/// channel's codebook being 14 or 15, and a band coded that way is never also
/// M/S. So the two loops below cannot both touch the same band, whatever order
/// they run in.
void apply_joint_stereo(State& st, unsigned left_index, unsigned right_index) noexcept
{
    State::Chan& left = st.chan[left_index];
    State::Chan& right = st.chan[right_index];
    const Ics& ics = left.ics;
    const unsigned win_len = (ics.window_sequence == k_eight_short) ? k_short_len : k_frame_len;

    unsigned group_start = 0;
    for (unsigned g = 0; g < ics.num_window_groups; ++g) {
        for (unsigned sb = 0; sb < ics.max_sfb; ++sb) {
            const unsigned cb = right.sfb_cb[g][sb];
            const unsigned lo = ics.swb_offset[sb];
            const unsigned hi = ics.swb_offset[sb + 1];

            if (cb == k_cb_intensity || cb == k_cb_intensity2) {
                // The right channel is a scaled copy of the left. cb 15 keeps
                // the sign, cb 14 inverts it, and M/S on the band inverts it
                // again.
                float sign = (cb == k_cb_intensity) ? 1.0F : -1.0F;
                if (st.ms_used[left_index][g][sb]) {
                    sign = -sign;
                }
                const float scale =
                    sign * std::pow(0.5F, static_cast<float>(right.sf[g][sb]) * 0.25F);
                for (unsigned w = 0; w < ics.group_len[g]; ++w) {
                    const unsigned base = (group_start + w) * win_len;
                    for (unsigned k = lo; k < hi && base + k < k_frame_len; ++k) {
                        right.coeffs[base + k] = left.coeffs[base + k] * scale;
                    }
                }
                continue;
            }

            if (!st.ms_used[left_index][g][sb] || cb == k_cb_noise) {
                continue;
            }
            for (unsigned w = 0; w < ics.group_len[g]; ++w) {
                const unsigned base = (group_start + w) * win_len;
                for (unsigned k = lo; k < hi && base + k < k_frame_len; ++k) {
                    const float mid = left.coeffs[base + k];
                    const float side = right.coeffs[base + k];
                    left.coeffs[base + k] = mid + side;
                    right.coeffs[base + k] = mid - side;
                }
            }
        }
        group_start += ics.group_len[g];
    }
}

// --------------------------------------------------------------- filterbank

/// One frame of spectrum to one frame of samples.
///
/// AAC's transform is lapped: each IMDCT produces twice as many samples as the
/// frame is long, and a frame is only finished once the next one has been added
/// to its tail. `overlap` carries that tail between calls, which is why a
/// decoder cannot be asked for frame N without having decoded N-1 -- and why
/// seeking to anywhere but the start of a track needs a frame of warm-up.
///
/// The left half of every window uses the *previous* frame's window shape and
/// the right half uses this one's. That is not symmetry for its own sake: the
/// two halves have to be the same curve where they overlap or the reconstruction
/// does not cancel.
void filterbank(const Ics& ics, const float* coeffs, float* overlap, unsigned prev_shape,
                float* out) noexcept
{
    float z[2 * k_frame_len] = {};
    const float* w_prev_long = window_for(prev_shape, false);
    const float* w_cur_long = window_for(ics.window_shape, false);
    const float* w_prev_short = window_for(prev_shape, true);
    const float* w_cur_short = window_for(ics.window_shape, true);

    if (ics.window_sequence == k_eight_short) {
        float tmp[2 * k_short_len];
        for (unsigned w = 0; w < k_windows; ++w) {
            imdct(coeffs + w * k_short_len, tmp, 2 * k_short_len);
            // Window w's rising half meets window w-1's falling half, so it uses
            // the previous *window's* shape -- which for the first is the
            // previous frame's.
            const float* rise = (w == 0) ? w_prev_short : w_cur_short;
            for (unsigned n = 0; n < k_short_len; ++n) {
                tmp[n] *= rise[n];
                tmp[k_short_len + n] *= w_cur_short[k_short_len - 1 - n];
            }
            const unsigned at = 448u + w * k_short_len;
            for (unsigned n = 0; n < 2 * k_short_len; ++n) {
                z[at + n] += tmp[n];
            }
        }
    } else {
        imdct(coeffs, z, 2 * k_frame_len);
        switch (ics.window_sequence) {
        case k_long_start:
            for (unsigned n = 0; n < k_frame_len; ++n) {
                z[n] *= w_prev_long[n];
            }
            // 1024..1471 pass through, 1472..1599 fall on a short window,
            // 1600..2047 are zero: this is the frame that hands over to eight
            // short ones.
            for (unsigned n = 0; n < k_short_len; ++n) {
                z[1472 + n] *= w_cur_short[k_short_len - 1 - n];
            }
            for (unsigned n = 1600; n < 2 * k_frame_len; ++n) {
                z[n] = 0.0F;
            }
            break;
        case k_long_stop:
            for (unsigned n = 0; n < 448; ++n) {
                z[n] = 0.0F;
            }
            for (unsigned n = 0; n < k_short_len; ++n) {
                z[448 + n] *= w_prev_short[n];
            }
            // 576..1023 pass through.
            for (unsigned n = 0; n < k_frame_len; ++n) {
                z[k_frame_len + n] *= w_cur_long[k_frame_len - 1 - n];
            }
            break;
        default: // ONLY_LONG
            for (unsigned n = 0; n < k_frame_len; ++n) {
                z[n] *= w_prev_long[n];
                z[k_frame_len + n] *= w_cur_long[k_frame_len - 1 - n];
            }
            break;
        }
    }

    // Full scale, and the one constant in this file that is a convention rather
    // than arithmetic.
    //
    // The standard's inverse quantisation puts samples on a scale where full
    // scale is 32768 -- it was written when the output was a 16-bit integer.
    // Float output is that divided by 2^15. Measured against FFmpeg on the same
    // file the fitted ratio came out at 3.05175748667e-05, whose base-two
    // logarithm is 15.000000, so this is the convention and not a fudge.
    constexpr float k_full_scale = 1.0F / 32768.0F;

    for (unsigned n = 0; n < k_frame_len; ++n) {
        out[n] = (z[n] + overlap[n]) * k_full_scale;
        overlap[n] = z[k_frame_len + n];
    }
}

} // namespace


Decoder::Decoder() noexcept = default;
Decoder::~Decoder() noexcept = default;

bool Decoder::init(const Config& cfg) noexcept
{
    ready_ = false;
    error_ = "";
    if (state_ == nullptr) {
        state_.reset(new (std::nothrow) State());
        if (state_ == nullptr) {
            error_ = "out of memory";
            return false;
        }
    }
    build_windows();
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
    for (unsigned c = 0; c < k_max_channels; ++c) {
        std::memset(overlap_[c], 0, sizeof(overlap_[c]));
        prev_shape_[c] = 0;
    }
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

    State& st = *state_;
    for (unsigned c = 0; c < k_max_channels; ++c) {
        st.pair_of[c] = -1;
    }

    auto read_ics = [&](unsigned index, bool have_common, const Ics& common) -> bool {
        State::Chan& ch = st.chan[index];
        Ics& ics = ch.ics;
        std::uint8_t (&sfb_cb)[k_windows][k_max_sfb] = ch.sfb_cb;
        int (&sf)[k_windows][k_max_sfb] = ch.sf;
        int* quant = st.quant;
        ch.tns = Tns{};
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
            if (!read_tns_data(r, ics, ch.tns)) {
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

        dequantise(ics, sfb_cb, sf, quant, ch.coeffs);
        apply_pns(ics, sfb_cb, sf, rng_, ch.coeffs);
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
            const Ics unused;
            if (!read_ics(channels_, false, unused)) {
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
                for (unsigned g = 0; g < common.num_window_groups; ++g) {
                    for (unsigned sb = 0; sb < common.max_sfb; ++sb) {
                        bool used = ms_mask_present == 2;
                        if (ms_mask_present == 1) {
                            used = r.bit() != 0;
                        }
                        st.ms_used[channels_][g][sb] = used;
                    }
                }
            }
            if (!read_ics(channels_, common_window, common)) {
                return false;
            }
            if (!read_ics(channels_ + 1, common_window, common)) {
                return false;
            }
            st.pair_of[channels_] = static_cast<int>(channels_ + 1);
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

    // Joint stereo, then TNS, then the filterbank -- the standard's order, and
    // not interchangeable: M/S and intensity work on the quantised spectrum's
    // scale, TNS shapes what comes out of that, and only then is it a signal.
    for (unsigned c = 0; c < channels_; ++c) {
        if (st.pair_of[c] >= 0) {
            apply_joint_stereo(st, c, static_cast<unsigned>(st.pair_of[c]));
        }
    }
    for (unsigned c = 0; c < channels_; ++c) {
        apply_tns(st.chan[c].ics, st.chan[c].tns, cfg_.rate_index, st.chan[c].coeffs);
        filterbank(st.chan[c].ics, st.chan[c].coeffs, overlap_[c], prev_shape_[c], pcm_[c]);
        last_sequence_[c] = st.chan[c].ics.window_sequence;
        prev_shape_[c] = st.chan[c].ics.window_shape;
    }
    return true;
}

} // namespace mp::aac

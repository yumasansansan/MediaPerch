// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC-LC, decoded here rather than by anything this project links to -- and in
// Rust, which is the second decision this file records.
//
// The first decision is why it is here at all. AAC is not an abandoned format
// and FAAD2 is not abandoned code; the problem was that every library
// available produced the wrong *thing* for this player:
//
//   * Media Foundation starts every AAC track 1024 frames -- 21.3 ms -- late,
//     leaves the encoder padding on the end, and refuses 8 kHz and 7.1 outright.
//   * FAAD2 discards two frames where the file says to discard one, so its
//     output begins 1024 frames into the audio. Four different ways of driving
//     it produced the identical wrong placement.
//   * libxaac is Apache-2.0, maintained, in OSS-Fuzz and shipped in Android --
//     and emits 16- or 24-bit integers only. AAC's inverse transform produces
//     real numbers; taking integers from a decoder is a quantisation performed
//     where nobody can see it.
//
// So this decoder exists to produce **float, framed exactly as the container
// says**. Both of those are decisions rather than arithmetic, and they are what
// the libraries got wrong.
//
// The second decision is the language, and the criterion for the port was the
// one ALAC's had: **not a sample may change.** This is a translation of the C++
// decoder that preceded it, function for function and in the same order of
// floating-point operations, and it was accepted only once every hash and every
// SNR figure docs/formats.md records came out the same. What did change is what
// a missed check costs: in C++ a spectral index past the frame was a write into
// whatever came next, and here it is a panic, caught at the module boundary and
// returned as a packet that failed to decode. `#![forbid(unsafe_code)]` is a
// crate-level fact the compiler enforces.
//
// Scope is AAC-LC. SBR (HE-AAC) and PS (HE-AACv2) are not here and are not
// planned: they are another six thousand lines apiece, and a file that needs
// them falls through to demux_ffmpeg, which is what the fallback chain is for.
//
// No OS calls, no allocation during decode, no I/O.

#![forbid(unsafe_code)]

mod tables;

use std::f64::consts::PI;
use std::fmt;
use std::sync::OnceLock;

pub const MAX_CHANNELS: usize = 8;
pub const FRAME_LEN: usize = 1024;
pub const SHORT_LEN: usize = 128;
pub const WINDOWS: usize = 8;
pub const MAX_SFB: usize = 51;
pub const MAX_TNS_ORDER: usize = 20;

/// FFmpeg's seed for the noise generator, so noise-substituted bands can be
/// compared with FFmpeg's output bit for bit. See `apply_pns` for why matching
/// it is a testing convenience rather than a requirement of the format.
pub const NOISE_SEED: u32 = 0x1f2e_3d4c;

/// Why a configuration or a frame was refused. A sentence, not a code: the
/// module logs it, and a person reads the log.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub &'static str);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for Error {}

// ---------------------------------------------------------------- constants

// The four window sequences. Only EIGHT_SHORT changes how a frame is *parsed*;
// the filterbank tells all four apart.
const LONG_START: u32 = 1;
const EIGHT_SHORT: u32 = 2;
const LONG_STOP: u32 = 3;

const ID_SCE: u32 = 0;
const ID_CPE: u32 = 1;
const ID_LFE: u32 = 3;
const ID_DSE: u32 = 4;
const ID_PCE: u32 = 5;
const ID_FIL: u32 = 6;
const ID_END: u32 = 7;

const CB_ZERO: u8 = 0;
const CB_ESC: u8 = 11;
const CB_NOISE: u8 = 13;
const CB_INTENSITY2: u8 = 14;
const CB_INTENSITY: u8 = 15;

const RATES: [u32; 13] = [
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
];

/// Dimension, modulo and offset for each spectrum codebook.
///
/// A codebook index is a number in base `modulus` with `dim` digits, each
/// digit minus `off` being one spectral value. That is the whole of the
/// mapping -- 81 = 3^4, 64 = 8^2, 169 = 13^2, 289 = 17^2 -- so no table of
/// tuples is needed, only arithmetic. `unsigned_cb` says whether a sign bit
/// follows each non-zero value.
struct BookShape {
    dim: usize,
    modulus: i32,
    off: i32,
    unsigned_cb: bool,
}

const BOOKS: [BookShape; 12] = [
    BookShape {
        dim: 0,
        modulus: 1,
        off: 0,
        unsigned_cb: false,
    }, // 0: ZERO, never decoded
    BookShape {
        dim: 4,
        modulus: 3,
        off: 1,
        unsigned_cb: false,
    },
    BookShape {
        dim: 4,
        modulus: 3,
        off: 1,
        unsigned_cb: false,
    },
    BookShape {
        dim: 4,
        modulus: 3,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 4,
        modulus: 3,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 2,
        modulus: 9,
        off: 4,
        unsigned_cb: false,
    },
    BookShape {
        dim: 2,
        modulus: 9,
        off: 4,
        unsigned_cb: false,
    },
    BookShape {
        dim: 2,
        modulus: 8,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 2,
        modulus: 8,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 2,
        modulus: 13,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 2,
        modulus: 13,
        off: 0,
        unsigned_cb: true,
    },
    BookShape {
        dim: 2,
        modulus: 17,
        off: 0,
        unsigned_cb: true,
    }, // 11: the escape codebook
];

/// The sample rate for one of the standard's 13 indices, or 0.
pub fn rate_for_index(index: u32) -> u32 {
    RATES.get(index as usize).copied().unwrap_or(0)
}

// --------------------------------------------------------------- bit reader

/// MSB-first, and it cannot leave the packet.
///
/// Reading past the end yields zeroes and sets `overrun`, which every caller
/// checks once at the end of the frame rather than after every field. That is
/// deliberate: a syntax error in AAC usually shows up as a read that runs off
/// the end several fields later, so the useful question is "did this frame end
/// where its packet ends", not "did this particular field fit".
struct BitReader<'a> {
    data: &'a [u8],
    bits: usize,
    pos: usize,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        BitReader {
            data,
            bits: data.len() * 8,
            pos: 0,
        }
    }

    fn pos(&self) -> usize {
        self.pos
    }

    fn size(&self) -> usize {
        self.bits
    }

    fn overrun(&self) -> bool {
        self.pos > self.bits
    }

    fn left(&self) -> usize {
        self.bits.saturating_sub(self.pos)
    }

    fn read(&mut self, n: u32) -> u32 {
        let mut v = 0u32;
        for _ in 0..n {
            v = (v << 1) | self.bit();
        }
        v
    }

    fn bit(&mut self) -> u32 {
        if self.pos >= self.bits {
            self.pos += 1;
            return 0;
        }
        let v = (self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1;
        self.pos += 1;
        u32::from(v)
    }

    fn skip(&mut self, n: usize) {
        self.pos = self.pos.saturating_add(n);
    }

    fn byte_align(&mut self) {
        self.pos = (self.pos + 7) & !7usize;
    }
}

// -------------------------------------------------------------------- VLC
//
// A binary trie, because AAC's codebooks are not canonical.
//
// The standard lists each codebook in the order of the tuple it decodes to and
// assigns codewords freely; they are prefix-free but they are not consecutive
// within a length, so the usual canonical "shift a bit in and compare a range"
// decode does not apply. A trie makes no assumption at all: it is built from
// (length, codeword) and it fails to build if any codeword is a prefix of
// another, which is the only property the tables actually promise. Roughly six
// hundred nodes for the largest codebook, walked one bit at a time.

const VLC_MAX_LEN: u32 = 24;
const VLC_MAX_NODES: usize = 1400;

#[derive(Clone, Copy)]
struct Node {
    child: [i16; 2],
    symbol: i16,
}

const EMPTY_NODE: Node = Node {
    child: [-1, -1],
    symbol: -1,
};

struct Vlc {
    node: Vec<Node>,
}

impl Vlc {
    /// `codes` is 32-bit because it has to be: the spectrum codebooks fit in
    /// sixteen, and the scalefactor codebook does not -- its longest codeword
    /// is nineteen bits and its largest value is 524287.
    fn build(codes: &[u32], bits: &[u8]) -> Option<Vlc> {
        let mut node = vec![EMPTY_NODE; VLC_MAX_NODES];
        let mut used = 1usize;

        for (i, (&code, &len)) in codes.iter().zip(bits).enumerate() {
            let len = u32::from(len);
            if len == 0 || len > VLC_MAX_LEN {
                return None;
            }
            let mut at = 0usize;
            for b in 0..len {
                let bit = ((code >> (len - 1 - b)) & 1) as usize;
                if node[at].symbol >= 0 {
                    return None; // a shorter codeword is a prefix of this one
                }
                if node[at].child[bit] < 0 {
                    if used >= VLC_MAX_NODES {
                        return None;
                    }
                    node[used] = EMPTY_NODE;
                    node[at].child[bit] = used as i16;
                    used += 1;
                }
                at = node[at].child[bit] as usize;
            }
            if node[at].symbol >= 0 || node[at].child[0] >= 0 || node[at].child[1] >= 0 {
                return None; // duplicate, or this codeword is a prefix of another
            }
            node[at].symbol = i as i16;
        }
        Some(Vlc { node })
    }

    /// The symbol, or -1 if the bits do not spell one.
    fn decode(&self, r: &mut BitReader) -> i32 {
        let mut at = 0usize;
        for _ in 0..=VLC_MAX_LEN {
            if self.node[at].symbol >= 0 {
                return i32::from(self.node[at].symbol);
            }
            let next = self.node[at].child[r.bit() as usize];
            if next < 0 {
                return -1;
            }
            at = next as usize;
        }
        -1
    }
}

// ------------------------------------------------------------- the tables
//
// Built once, on first use, and shared by every decoder: the twelve tries, the
// four windows and the two cosine tables. A `OnceLock` rather than a static
// initialiser because the tries are built from the standard's data at run
// time, and because building them can fail -- which is reported by the first
// `Decoder::new` rather than by a crash in a constructor.

/// The zeroth-order modified Bessel function, for the KBD window.
fn bessel_i0(x: f64) -> f64 {
    let mut sum = 1.0;
    let mut term = 1.0;
    for i in 1..50 {
        let i = f64::from(i);
        term *= (x * x) / (4.0 * i * i);
        sum += term;
        if term < sum * 1e-17 {
            break;
        }
    }
    sum
}

/// Sine and Kaiser-Bessel-derived windows, long and short, and the cosine
/// tables the transform reads.
///
/// The windows are computed rather than tabulated: the sine window is a sine,
/// and KBD is a running sum of a Kaiser window normalised and square-rooted.
/// Tabulating them would add two thousand more constants to check.
struct Tables {
    /// Index 0 is a placeholder with no codewords; codebooks 1 to 11 decode.
    spectrum: Vec<Vlc>,
    scalefactor: Vlc,
    sine_long: Vec<f32>,
    sine_short: Vec<f32>,
    kbd_long: Vec<f32>,
    kbd_short: Vec<f32>,
    /// cos(pi * m / (4N)) for m < 4N, the argument the IMDCT needs. Double,
    /// and the accumulator is too: a float table costs about ten decibels
    /// against FFmpeg -- 123 rather than 135 -- because a thousand products
    /// are summed per output sample and the table's own rounding is inside
    /// every one of them.
    cos_long: Box<[f64; 8 * FRAME_LEN]>,
    cos_short: Box<[f64; 8 * SHORT_LEN]>,
}

fn build_kbd(n: usize, alpha: f64) -> Vec<f32> {
    // w[j] = I0(pi*alpha*sqrt(1 - (2j/n - 1)^2)), then the running sum of those,
    // normalised by the total and square-rooted.
    let mut kaiser = vec![0.0f64; n + 1];
    let mut total = 0.0f64;
    for (j, k) in kaiser.iter_mut().enumerate() {
        let x = 2.0 * j as f64 / n as f64 - 1.0;
        *k = bessel_i0(PI * alpha * (1.0 - x * x).sqrt());
        total += *k;
    }
    let mut out = vec![0.0f32; n];
    let mut running = 0.0f64;
    for (i, o) in out.iter_mut().enumerate() {
        running += kaiser[i];
        *o = (running / total).sqrt() as f32;
    }
    out
}

fn build_tables() -> Option<Tables> {
    let mut spectrum = Vec::with_capacity(12);
    spectrum.push(Vlc::build(&[], &[])?);
    for book in &tables::SPECTRAL {
        let wide: Vec<u32> = book.codes.iter().map(|&c| u32::from(c)).collect();
        spectrum.push(Vlc::build(&wide, book.bits)?);
    }
    let scalefactor = Vlc::build(&tables::SCALEFACTOR_CODE, &tables::SCALEFACTOR_BITS)?;

    // W_SIN_LEFT,N(i) = sin(pi/N (i + 1/2)) for 0 <= i < N/2, with N the whole
    // window length -- so a quarter sine rising from ~0 to 1 across the half,
    // not a half sine peaking in the middle. The difference is invisible until
    // it is checked against Princen-Bradley: the right one satisfies
    // w[i]^2 + w[N/2-1-i]^2 == 1 and the wrong one does not, which is exactly
    // the condition the overlap relies on to cancel.
    let sine_long: Vec<f32> = (0..FRAME_LEN)
        .map(|i| (PI / (2.0 * FRAME_LEN as f64) * (i as f64 + 0.5)).sin() as f32)
        .collect();
    let sine_short: Vec<f32> = (0..SHORT_LEN)
        .map(|i| (PI / (2.0 * SHORT_LEN as f64) * (i as f64 + 0.5)).sin() as f32)
        .collect();
    let kbd_long = build_kbd(FRAME_LEN, 4.0);
    let kbd_short = build_kbd(SHORT_LEN, 6.0);

    let cos_long: Vec<f64> = (0..8 * FRAME_LEN)
        .map(|m| (PI * m as f64 / (4.0 * FRAME_LEN as f64)).cos())
        .collect();
    let cos_short: Vec<f64> = (0..8 * SHORT_LEN)
        .map(|m| (PI * m as f64 / (4.0 * SHORT_LEN as f64)).cos())
        .collect();
    let cos_long: Box<[f64; 8 * FRAME_LEN]> = cos_long.into_boxed_slice().try_into().ok()?;
    let cos_short: Box<[f64; 8 * SHORT_LEN]> = cos_short.into_boxed_slice().try_into().ok()?;

    Some(Tables {
        spectrum,
        scalefactor,
        sine_long,
        sine_short,
        kbd_long,
        kbd_short,
        cos_long,
        cos_short,
    })
}

static TABLES: OnceLock<Option<Tables>> = OnceLock::new();

fn tables() -> Result<&'static Tables, Error> {
    TABLES
        .get_or_init(build_tables)
        .as_ref()
        .ok_or(Error("a Huffman codebook is not a prefix code"))
}

impl Tables {
    fn window(&self, shape: u32, is_short: bool) -> &[f32] {
        match (is_short, shape != 0) {
            (true, true) => &self.kbd_short,
            (true, false) => &self.sine_short,
            (false, true) => &self.kbd_long,
            (false, false) => &self.sine_long,
        }
    }
}

/// The inverse MDCT.
///
/// Still the definition -- one cosine sum per output sample -- but the cosine is
/// a table lookup rather than a call. The argument
///
/// ```text
/// 2*pi/N (i + n0)(k + 1/2),  with n0 = (N/2 + 1)/2
/// ```
///
/// is not an integer multiple of anything convenient, but twice it is: doubling
/// gives pi/(2N) * (2i + N/2 + 1) * (2k + 1), whose two factors are both whole
/// numbers. One table of cos(pi*m/(2N)) over m < 4N therefore answers every one
/// of them, and the index advances by a constant stride as k does.
///
/// It is still O(N^2). An FFT would make it O(N log N) and is worth doing when
/// there is a reason; the definition is what can be read against the standard,
/// and this keeps that while making it usable.
///
/// **The table is an array of a power-of-two size, and the index is masked.**
/// That is what lets the inner loop -- two million iterations per long window
/// -- run without a bounds check: the compiler can see that `idx & (M - 1)` is
/// below `M`, where it cannot see that a wrapped counter is. Measured before
/// this shape, the Rust decoder took 1.5 times as long as the C++ on the same
/// file. The value is the same either way: `idx` and `step` are both below `M`
/// and `M` is a power of two, so the mask is the modulo.
fn imdct<const M: usize>(table: &[f64; M], spec: &[f32], out: &mut [f32]) {
    const { assert!(M.is_power_of_two()) };
    let n = M / 4;
    let half = n / 2;
    let scale = 2.0 / n as f64;

    for (i, o) in out[..n].iter_mut().enumerate() {
        let a = 2 * i + half + 1; // 2i + N/2 + 1, and half *is* N/2
        let mut idx = a & (M - 1); // k = 0, so (2k + 1) = 1
        let step = (2 * a) & (M - 1);
        let mut acc = 0.0f64;
        for &s in &spec[..half] {
            acc += f64::from(s) * table[idx];
            idx = (idx + step) & (M - 1);
        }
        *o = (acc * scale) as f32;
    }
}

// ----------------------------------------------------------------- layout

/// Channels in the order AAC's elements produce them, mapped onto WAVE slots.
///
/// A decoder hands channels back in the order their elements appeared, which for
/// 5.1 is C, L, R, Ls, Rs, LFE -- centre first, because the single-channel
/// element comes first. WAVE wants L, R, C, LFE, Ls, Rs. Without this table a
/// 5.1 decode measures -3 dB against a reference and sounds like the centre
/// channel wandered, which is exactly what Media Foundation does to ALAC.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ChannelLayout {
    /// MP_SPEAKER_* bits, 0 for mono and stereo.
    pub mask: u32,
    /// 0 if this configuration has no layout here.
    pub count: u8,
    /// For each WAVE slot, the decoded channel that fills it.
    pub from: [u8; 8],
}

/// Assigns speakers to the channels a program config element describes.
///
/// A PCE gives counts and pair flags, not positions: so many front elements, so
/// many side, so many back, and whether each is a pair. The order the decoder
/// produces channels in is exactly that order, and the speakers follow from the
/// standard's rule that elements are listed **from the centre outwards** -- a
/// leading single front element is the centre, front pairs run inwards-first (so
/// with two of them the first is left and right *of centre* and the second is
/// the main pair), side pairs are the sides, back pairs the backs, a single back
/// element is the back centre, and the LFE is last.
///
/// That covers every layout an encoder writes configuration 0 for, including
/// FFmpeg's 7.1(wide). Anything it cannot place leaves `count` at zero and the
/// file goes to a decoder that can.
fn layout_from_pce(
    front: usize,
    front_cpe: &[bool; 16],
    side: usize,
    side_cpe: &[bool; 16],
    back: usize,
    back_cpe: &[bool; 16],
    lfe: usize,
) -> ChannelLayout {
    let none = ChannelLayout::default();
    let mut speaker = [0u32; MAX_CHANNELS];
    let mut n = 0usize;
    let mut place = |bit: u32| {
        if n < MAX_CHANNELS {
            speaker[n] = bit;
            n += 1;
        }
    };

    // Front elements are listed from the centre outwards, so which speaker a
    // pair belongs to depends on how many pairs there are: one pair is the main
    // left and right, and where there are two the *first* is the inner pair --
    // front left and right of centre -- and the second is the main pair. Read
    // the other way round, 7.1(wide) decodes with its front channels swapped and
    // measures -3 dB against a reference while every individual channel is
    // perfect, which is how this was found.
    let front_pairs = front_cpe[..front.min(16)].iter().filter(|&&p| p).count();
    if front_pairs > 2 {
        return none; // more front pairs than there are names for
    }
    let mut pair_seen = 0usize;
    for &is_pair in &front_cpe[..front.min(16)] {
        if !is_pair {
            place(0x4); // front centre
            continue;
        }
        let inner = front_pairs == 2 && pair_seen == 0;
        place(if inner { 0x40 } else { 0x1 }); // front left of centre, or front left
        place(if inner { 0x80 } else { 0x2 });
        pair_seen += 1;
    }
    for &is_pair in &side_cpe[..side.min(16)] {
        if !is_pair {
            return none;
        }
        place(0x200);
        place(0x400);
    }
    for &is_pair in &back_cpe[..back.min(16)] {
        if is_pair {
            place(0x10);
            place(0x20);
        } else {
            place(0x100); // back centre
        }
    }
    for _ in 0..lfe {
        place(0x8);
    }

    if n == 0 || n > MAX_CHANNELS {
        return none;
    }
    let mut mask = 0u32;
    for &s in &speaker[..n] {
        if s == 0 || (mask & s) != 0 {
            return none; // unplaceable, or the same speaker twice
        }
        mask |= s;
    }

    // WAVE order is ascending speaker bit.
    let mut out = ChannelLayout::default();
    let mut slot = 0usize;
    for b in 0..32 {
        let bit = 1u32 << b;
        if (mask & bit) == 0 {
            continue;
        }
        if let Some(c) = speaker[..n].iter().position(|&s| s == bit) {
            out.from[slot] = c as u8;
            slot += 1;
        }
    }
    out.mask = if n <= 2 { 0 } else { mask };
    out.count = n as u8;
    out
}

/// Reads a program_config_element and turns it into a layout.
///
/// The whole element is consumed either way, because in a raw_data_block the
/// bits after it belong to the next element and the count has to stay honest.
/// `count` is left at 0 when the layout is one this decoder cannot name.
fn read_pce(r: &mut BitReader) -> ChannelLayout {
    r.read(4); // element_instance_tag
    r.read(2); // object_type
    r.read(4); // sampling_frequency_index
    let front = r.read(4) as usize;
    let side = r.read(4) as usize;
    let back = r.read(4) as usize;
    let lfe = r.read(2) as usize;
    let assoc = r.read(3) as usize;
    let cc = r.read(4) as usize;
    if r.bit() != 0 {
        r.read(4); // mono_mixdown_element_number
    }
    if r.bit() != 0 {
        r.read(4); // stereo_mixdown_element_number
    }
    if r.bit() != 0 {
        r.read(3); // matrix_mixdown_idx and pseudo_surround_enable
    }
    let mut front_cpe = [false; 16];
    let mut side_cpe = [false; 16];
    let mut back_cpe = [false; 16];
    for i in 0..front {
        front_cpe[i & 15] = r.bit() != 0;
        r.read(4);
    }
    for i in 0..side {
        side_cpe[i & 15] = r.bit() != 0;
        r.read(4);
    }
    for i in 0..back {
        back_cpe[i & 15] = r.bit() != 0;
        r.read(4);
    }
    for _ in 0..lfe + assoc {
        r.read(4);
    }
    for _ in 0..cc {
        r.read(5);
    }
    r.byte_align();
    let comment = r.read(8) as usize;
    r.skip(comment * 8);

    if r.overrun() {
        ChannelLayout::default()
    } else {
        layout_from_pce(front, &front_cpe, side, &side_cpe, back, &back_cpe, lfe)
    }
}

/// The layout for channel configurations 1 to 7, 11 and 12, and an empty one
/// for the rest.
pub fn layout_for_config(channel_config: u32) -> ChannelLayout {
    // Decoded order per configuration, from ISO/IEC 14496-3 Table 1.19:
    //   1  C                            2  L R
    //   3  C L R                        4  C L R Cs
    //   5  C L R Ls Rs                  6  C L R Ls Rs LFE
    //   7  C L R Ls Rs Lr Rr LFE       11  C L R Ls Rs Cs LFE
    //  12  C L R Ls Rs Lr Rr LFE
    //
    // `from[slot]` is the decoded channel that belongs in that WAVE slot, with
    // the slots in ascending SPEAKER_* bit order.
    const fn layout(mask: u32, count: u8, from: [u8; 8]) -> ChannelLayout {
        ChannelLayout { mask, count, from }
    }
    const NONE: ChannelLayout = layout(0, 0, [0; 8]);
    const TABLE: [ChannelLayout; 15] = [
        /* 0  */ NONE,
        /* 1  */ layout(0, 1, [0, 0, 0, 0, 0, 0, 0, 0]),
        /* 2  */ layout(0, 2, [0, 1, 0, 0, 0, 0, 0, 0]),
        /* 3  */ layout(0x1 | 0x2 | 0x4, 3, [1, 2, 0, 0, 0, 0, 0, 0]),
        /* 4  */ layout(0x1 | 0x2 | 0x4 | 0x100, 4, [1, 2, 0, 3, 0, 0, 0, 0]),
        /* 5  */ layout(0x1 | 0x2 | 0x4 | 0x10 | 0x20, 5, [1, 2, 0, 3, 4, 0, 0, 0]),
        /* 6  */
        layout(
            0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20,
            6,
            [1, 2, 0, 5, 3, 4, 0, 0],
        ),
        /* 7  */
        layout(
            0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400,
            8,
            [1, 2, 0, 7, 5, 6, 3, 4],
        ),
        /* 8  */ NONE,
        /* 9  */ NONE,
        /* 10 */ NONE,
        /* 11 */
        layout(
            0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400,
            7,
            [1, 2, 0, 6, 5, 3, 4, 0],
        ),
        /* 12 */
        layout(
            0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400,
            8,
            [1, 2, 0, 7, 5, 6, 3, 4],
        ),
        /* 13 */ NONE,
        /* 14 */ NONE,
    ];
    TABLE[if channel_config < 15 {
        channel_config as usize
    } else {
        0
    }]
}

// ------------------------------------------------------------------ config

/// The AudioSpecificConfig, as far as AAC-LC needs it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Config {
    /// 2 is AAC-LC, and the only one accepted.
    pub object_type: u32,
    /// Index into the standard's 13 rates.
    pub rate_index: u32,
    pub sample_rate: u32,
    /// 0 means "read a program config element".
    pub channel_config: u32,
    /// frameLengthFlag; 960-sample frames.
    pub frame_960: bool,
    /// Filled when `channel_config` is 0 and the AudioSpecificConfig carried a
    /// program config element -- which is where FFmpeg's encoder puts the layout
    /// for 7.1(wide) in an MP4. `count` is 0 when there was no PCE, as in a raw
    /// ADTS stream, where the element arrives inside each frame instead.
    pub pce: ChannelLayout,
}

impl Config {
    pub fn parse(asc: &[u8]) -> Result<Config, Error> {
        if asc.len() < 2 {
            return Err(Error("too short to be an AudioSpecificConfig"));
        }
        let mut r = BitReader::new(asc);

        let mut object_type = r.read(5);
        if object_type == 31 {
            object_type = 32 + r.read(6);
        }
        let rate_index = r.read(4);
        let rate = if rate_index == 15 {
            r.read(24)
        } else {
            rate_for_index(rate_index)
        };
        let channel_config = r.read(4);

        // SBR and PS are signalled by wrapping AAC-LC in object type 5 or 29.
        // This decoder does not implement either, and saying so here rather
        // than producing the core at half the rate is the honest answer: the
        // file goes to demux_ffmpeg instead.
        if object_type == 5 || object_type == 29 {
            return Err(Error("HE-AAC and HE-AACv2 are not decoded here"));
        }
        if object_type != 2 {
            return Err(Error("not AAC-LC"));
        }
        // 0..7 are the classic configurations; 11..14 were added later and 12
        // is 7.1. Anything else is not a layout this decoder has a mapping for.
        if rate == 0 {
            return Err(Error("no sample rate"));
        }
        if (8..=10).contains(&channel_config) || channel_config > 14 {
            return Err(Error("a channel configuration with no layout"));
        }

        // GASpecificConfig
        let frame_960 = r.bit() != 0;
        if r.bit() != 0 {
            return Err(Error("dependsOnCoreCoder is not AAC-LC"));
        }
        if r.bit() != 0 {
            return Err(Error("an extension flag is not defined for AAC-LC"));
        }
        // GASpecificConfig puts the program config element right here when the
        // configuration field was 0, which is the only place an MP4 carries it.
        let mut pce = ChannelLayout::default();
        if channel_config == 0 {
            pce = read_pce(&mut r);
            if pce.count == 0 {
                return Err(Error(
                    "configuration 0 without a program config element this decoder can place",
                ));
            }
        }
        if r.overrun() {
            return Err(Error("the AudioSpecificConfig ended early"));
        }

        Ok(Config {
            object_type,
            rate_index: if rate_index < 13 { rate_index } else { 0 },
            sample_rate: rate,
            channel_config,
            frame_960,
            pce,
        })
    }
}

// ------------------------------------------------------------- frame state

/// One TNS filter, as read and then as coefficients.
#[derive(Clone, Copy)]
struct TnsFilter {
    order: u32,
    length: u32,
    direction: u32,
    lpc: [f32; MAX_TNS_ORDER + 1],
}

const NO_FILTER: TnsFilter = TnsFilter {
    order: 0,
    length: 0,
    direction: 0,
    lpc: [0.0; MAX_TNS_ORDER + 1],
};

#[derive(Clone, Copy)]
struct Tns {
    present: bool,
    n_filt: [usize; WINDOWS],
    filt: [[TnsFilter; 4]; WINDOWS],
}

const NO_TNS: Tns = Tns {
    present: false,
    n_filt: [0; WINDOWS],
    filt: [[NO_FILTER; 4]; WINDOWS],
};

#[derive(Clone, Copy)]
struct Ics {
    window_sequence: u32,
    window_shape: u32,
    max_sfb: usize,
    num_windows: usize,
    num_window_groups: usize,
    group_len: [usize; WINDOWS],
    num_swb: usize,
    /// Band edges, `num_swb + 1` of them, the last being the window length.
    swb_offset: &'static [u16],
}

const NO_ICS: Ics = Ics {
    window_sequence: 0,
    window_shape: 0,
    max_sfb: 0,
    num_windows: 1,
    num_window_groups: 1,
    group_len: [1, 0, 0, 0, 0, 0, 0, 0],
    num_swb: 0,
    swb_offset: &[],
};

impl Ics {
    fn is_short(&self) -> bool {
        self.window_sequence == EIGHT_SHORT
    }

    fn win_len(&self) -> usize {
        if self.is_short() {
            SHORT_LEN
        } else {
            FRAME_LEN
        }
    }
}

/// Per-channel working state, one frame's worth.
struct Chan {
    ics: Ics,
    tns: Tns,
    sfb_cb: [[u8; MAX_SFB]; WINDOWS],
    sf: [[i32; MAX_SFB]; WINDOWS],
    coeffs: [f32; FRAME_LEN],
}

impl Chan {
    fn new() -> Chan {
        Chan {
            ics: NO_ICS,
            tns: NO_TNS,
            sfb_cb: [[0; MAX_SFB]; WINDOWS],
            sf: [[0; MAX_SFB]; WINDOWS],
            coeffs: [0.0; FRAME_LEN],
        }
    }
}

/// Everything a frame needs while it is being decoded, held by the decoder
/// rather than by anything shared. It is a lot of memory -- eight channels of
/// spectrum, scale factors, codebook assignments and TNS state -- which is why
/// it is on the heap.
struct State {
    chan: [Chan; MAX_CHANNELS],
    quant: [i32; FRAME_LEN],
    /// For a channel pair: the second channel of the pair, or -1.
    pair_of: [i32; MAX_CHANNELS],
    ms_used: [[[bool; MAX_SFB]; WINDOWS]; MAX_CHANNELS],
}

impl State {
    fn new() -> Box<State> {
        Box::new(State {
            chan: std::array::from_fn(|_| Chan::new()),
            quant: [0; FRAME_LEN],
            pair_of: [-1; MAX_CHANNELS],
            ms_used: [[[false; MAX_SFB]; WINDOWS]; MAX_CHANNELS],
        })
    }
}

// ------------------------------------------------------------ the syntax

fn read_ics_info(r: &mut BitReader, cfg: &Config, ics: &mut Ics) -> bool {
    let ri = cfg.rate_index as usize;
    r.bit(); // ics_reserved_bit
    ics.window_sequence = r.read(2);
    ics.window_shape = r.bit();

    if ics.window_sequence == EIGHT_SHORT {
        ics.max_sfb = r.read(4) as usize;
        ics.num_windows = WINDOWS;
        ics.num_window_groups = 1;
        ics.group_len[0] = 1;
        for _ in 1..WINDOWS {
            if r.bit() != 0 {
                ics.group_len[ics.num_window_groups - 1] += 1;
            } else {
                ics.group_len[ics.num_window_groups] = 1;
                ics.num_window_groups += 1;
            }
        }
        ics.num_swb = usize::from(tables::NUM_SWB_128[ri]);
        ics.swb_offset = tables::SWB_OFFSET_128[ri];
    } else {
        ics.max_sfb = r.read(6) as usize;
        ics.num_windows = 1;
        ics.num_window_groups = 1;
        ics.group_len[0] = 1;
        if r.bit() != 0 {
            // predictor_data_present. Main-profile prediction is not AAC-LC and
            // this decoder will not guess at it.
            return false;
        }
        ics.num_swb = usize::from(tables::NUM_SWB_1024[ri]);
        ics.swb_offset = tables::SWB_OFFSET_1024[ri];
    }

    ics.max_sfb <= ics.num_swb && ics.max_sfb <= MAX_SFB
}

/// Section data: which codebook covers which scalefactor bands.
// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn read_section_data(r: &mut BitReader, ics: &Ics, sfb_cb: &mut [[u8; MAX_SFB]; WINDOWS]) -> bool {
    let bits = if ics.is_short() { 3 } else { 5 };
    let escape = (1u32 << bits) - 1;

    for g in 0..ics.num_window_groups {
        let mut k = 0usize;
        while k < ics.max_sfb {
            // Past the end every read returns zero, so a zero-length section
            // would spin here forever without this. The check belongs in the
            // loop that can fail to advance, not only in the one that reads.
            if r.overrun() {
                return false;
            }
            let cb = r.read(4);
            if cb == 12 {
                return false; // reserved
            }
            let mut len = 0usize;
            let mut incr = r.read(bits);
            while incr == escape {
                len += escape as usize;
                incr = r.read(bits);
                if r.overrun() {
                    return false;
                }
            }
            len += incr as usize;
            if k + len > ics.max_sfb {
                return false;
            }
            for _ in 0..len {
                sfb_cb[g][k] = cb as u8;
                k += 1;
            }
        }
    }
    true
}

/// Scale factors, intensity positions and noise energies, all differential.
// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn read_scale_factors(
    r: &mut BitReader,
    t: &Tables,
    ics: &Ics,
    sfb_cb: &[[u8; MAX_SFB]; WINDOWS],
    global_gain: i32,
    sf: &mut [[i32; MAX_SFB]; WINDOWS],
) -> bool {
    let mut scale = global_gain;
    let mut intensity = 0i32;
    let mut noise = global_gain - 90;
    let mut noise_started = false;

    for g in 0..ics.num_window_groups {
        for s in 0..ics.max_sfb {
            let cb = sfb_cb[g][s];
            if cb == CB_ZERO {
                sf[g][s] = 0;
                continue;
            }
            if cb == CB_INTENSITY || cb == CB_INTENSITY2 {
                let delta = t.scalefactor.decode(r) - 60;
                if delta < -60 {
                    return false;
                }
                intensity += delta;
                sf[g][s] = intensity;
                continue;
            }
            if cb == CB_NOISE {
                if !noise_started {
                    noise += r.read(9) as i32 - 256;
                    noise_started = true;
                } else {
                    let delta = t.scalefactor.decode(r) - 60;
                    if delta < -60 {
                        return false;
                    }
                    noise += delta;
                }
                sf[g][s] = noise;
                continue;
            }
            let delta = t.scalefactor.decode(r) - 60;
            if delta < -60 {
                return false;
            }
            scale += delta;
            if !(0..=255).contains(&scale) {
                return false;
            }
            sf[g][s] = scale;
        }
    }
    true
}

fn read_pulse_data(r: &mut BitReader, ics: &Ics) -> bool {
    let count = r.read(2) + 1;
    let pulse_start_sfb = r.read(6) as usize;
    if pulse_start_sfb >= ics.num_swb {
        return false;
    }
    for _ in 0..count {
        r.read(5); // pulse_offset
        r.read(4); // pulse_amp
    }
    true
}

/// Quantised reflection coefficients to LPC, by the standard's recursion.
///
/// The reflection values are not a table: the standard defines them as
/// `sin(q / iqfac)`, with a slightly different scale for negative q, so they are
/// computed. That is worth saying because it is the one place in AAC where a
/// table would have been expected and is not needed.
fn tns_coefficients(
    quant: &[i32; MAX_TNS_ORDER],
    order: usize,
    coef_res: u32,
    lpc: &mut [f32; MAX_TNS_ORDER + 1],
) {
    // The quantiser's resolution comes from `coef_res` alone. `coef_compress`
    // says only that fewer bits were *transmitted* -- the scale the values are
    // on does not change with it. Folding compress into this halves the divisor
    // whenever a filter is compressed and bends every reflection coefficient
    // slightly, which is invisible except as a few dB in the frames that use
    // TNS at all.
    let half = f64::from(1u32 << (coef_res + 2));
    let iqfac = (half - 0.5) / (PI / 2.0);
    let iqfac_m = (half + 0.5) / (PI / 2.0);

    let mut parcor = [0.0f64; MAX_TNS_ORDER];
    for i in 0..order {
        let q = f64::from(quant[i]);
        parcor[i] = (q / if q >= 0.0 { iqfac } else { iqfac_m }).sin();
    }

    let mut a = [0.0f64; MAX_TNS_ORDER + 1];
    let mut b = [0.0f64; MAX_TNS_ORDER + 1];
    for m in 1..=order {
        for i in 1..m {
            b[i] = a[i] + parcor[m - 1] * a[m - i];
        }
        a[1..m].copy_from_slice(&b[1..m]);
        a[m] = parcor[m - 1];
    }
    for i in 0..=order {
        lpc[i] = a[i] as f32;
    }
}

fn read_tns_data(r: &mut BitReader, ics: &Ics, tns: &mut Tns) -> bool {
    let is_short = ics.is_short();
    let n_filt_bits = if is_short { 1 } else { 2 };
    let length_bits = if is_short { 4 } else { 6 };
    let order_bits = if is_short { 3 } else { 5 };
    let max_order = if is_short { 7 } else { 20 };

    tns.present = true;
    for w in 0..ics.num_windows {
        let n_filt = r.read(n_filt_bits) as usize;
        tns.n_filt[w] = n_filt.min(4);
        let mut coef_res = 0u32;
        if n_filt != 0 {
            coef_res = r.bit();
        }
        for f in 0..n_filt {
            let length = r.read(length_bits);
            let order = r.read(order_bits) as usize;
            if order > max_order {
                return false;
            }
            let mut direction = 0u32;
            let mut quant = [0i32; MAX_TNS_ORDER];
            if order != 0 {
                direction = r.bit();
                let compress = r.bit();
                let coef_bits = coef_res + 3 - compress;
                for q in quant.iter_mut().take(order) {
                    let raw = r.read(coef_bits) as i32;
                    // Signed, in `coef_bits` bits.
                    let sign = 1i32 << (coef_bits - 1);
                    *q = raw - if (raw & sign) != 0 { sign << 1 } else { 0 };
                }
            }
            if f < 4 {
                let out = &mut tns.filt[w][f];
                out.order = order as u32;
                out.length = length;
                out.direction = direction;
                if order != 0 {
                    tns_coefficients(&quant, order, coef_res, &mut out.lpc);
                }
            }
        }
    }
    true
}

/// The inverse of the encoder's temporal noise shaping: an all-pole filter run
/// across frequency rather than time.
fn apply_tns(ics: &Ics, tns: &Tns, rate_index: usize, coeffs: &mut [f32; FRAME_LEN]) {
    if !tns.present {
        return;
    }
    let is_short = ics.is_short();
    // The filter reaches no further than the lesser of the rate's TNS limit and
    // the bands this frame actually coded. Clamping to only the first leaves the
    // filter running over bands the frame never sent, which is a small error
    // rather than a loud one -- it cost about 45 dB in the two frames of an 8 kHz
    // file that use TNS, and nothing anywhere else.
    let rate_limit = usize::from(if is_short {
        tables::TNS_MAX_BANDS_128[rate_index]
    } else {
        tables::TNS_MAX_BANDS_1024[rate_index]
    });
    let max_bands = rate_limit.min(ics.max_sfb);
    let win_len = ics.win_len();

    for w in 0..ics.num_windows {
        let mut bottom = ics.num_swb;
        for f in 0..tns.n_filt[w] {
            let filt = &tns.filt[w][f];
            let top = bottom;
            bottom = top.saturating_sub(filt.length as usize);
            if filt.order == 0 {
                continue;
            }
            let lo_band = bottom.min(max_bands);
            let hi_band = top.min(max_bands);
            if lo_band > ics.num_swb || hi_band > ics.num_swb {
                continue;
            }
            let mut start = i64::from(ics.swb_offset[lo_band]);
            let end = i64::from(ics.swb_offset[hi_band]);
            let size = end - start;
            if size <= 0 {
                continue;
            }
            let mut inc = 1i64;
            if filt.direction != 0 {
                inc = -1;
                start = end - 1;
            }
            start += (w * win_len) as i64;

            let order = filt.order as usize;
            for m in 0..size as usize {
                let taps = m.min(order);
                let mut acc = coeffs[start as usize];
                for i in 1..=taps {
                    acc -= coeffs[(start - i as i64 * inc) as usize] * filt.lpc[i];
                }
                coeffs[start as usize] = acc;
                start += inc;
            }
        }
    }
}

/// The spectrum itself, and the only place a codebook is used.
// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn read_spectral_data(
    r: &mut BitReader,
    t: &Tables,
    ics: &Ics,
    sfb_cb: &[[u8; MAX_SFB]; WINDOWS],
    quant: &mut [i32; FRAME_LEN],
) -> bool {
    quant.fill(0);

    let win_len = ics.win_len();
    let mut group_start = 0usize;

    for g in 0..ics.num_window_groups {
        let group_windows = ics.group_len[g];
        for s in 0..ics.max_sfb {
            let cb = sfb_cb[g][s];
            if cb == CB_ZERO || cb == CB_NOISE || cb == CB_INTENSITY || cb == CB_INTENSITY2 {
                continue;
            }
            if cb > 11 {
                return false;
            }
            let shape = &BOOKS[cb as usize];
            let lo = usize::from(ics.swb_offset[s]);
            let hi = usize::from(ics.swb_offset[s + 1]);
            if hi <= lo || hi > win_len {
                return false;
            }

            for w in 0..group_windows {
                let base = (group_start + w) * win_len;
                let mut k = lo;
                while k < hi {
                    let index = t.spectrum[cb as usize].decode(r);
                    if index < 0 {
                        return false;
                    }
                    // The index is a base-`modulus` number, most significant
                    // digit first; each digit less `off` is one spectral value.
                    let mut value = [0i32; 4];
                    let mut rest = index;
                    for d in (0..shape.dim).rev() {
                        value[d] = (rest % shape.modulus) - shape.off;
                        rest /= shape.modulus;
                    }
                    // Signs for the whole tuple first, and only then the
                    // escapes. That order is the standard's and it is not
                    // interchangeable: reading sign, escape, sign, escape puts
                    // the escape's bits where the second sign bit should be, and
                    // the frame desynchronises from there. It shows up only in
                    // frames that contain an escape at all, which is why most
                    // packets parsed perfectly while this was wrong.
                    if shape.unsigned_cb {
                        for v in value.iter_mut().take(shape.dim) {
                            if *v != 0 && r.bit() != 0 {
                                *v = -*v;
                            }
                        }
                    }
                    if cb == CB_ESC {
                        for v in value.iter_mut().take(shape.dim) {
                            if *v != 16 && *v != -16 {
                                continue;
                            }
                            let mut n = 4u32;
                            while r.bit() != 0 {
                                n += 1;
                                if n > 20 {
                                    return false;
                                }
                            }
                            let extra = r.read(n);
                            let magnitude = ((1u32 << n) | extra) as i32;
                            *v = if *v < 0 { -magnitude } else { magnitude };
                        }
                    }
                    for (d, &v) in value.iter().enumerate().take(shape.dim) {
                        if base + k + d < FRAME_LEN {
                            quant[base + k + d] = v;
                        }
                    }
                    if r.overrun() {
                        return false;
                    }
                    k += shape.dim;
                }
            }
        }
        group_start += group_windows;
    }
    true
}

/// Perceptual noise substitution: a band the encoder replaced with a level.
///
/// Codebook 13 says "there was noise here, this loud" and sends no coefficients
/// at all. A decoder that skips those bands emits silence across them, which on
/// the opening frame of a noise signal was 22 of 49 bands and showed up as
/// output eight times too quiet and uncorrelated with anything.
///
/// **The samples in such a band are arbitrary by design.** The standard fixes
/// the band's energy and says nothing about which noise fills it, so two
/// conformant decoders differ there and no comparison between them can be
/// sample-exact. The generator here is the one FFmpeg uses, seed included, for
/// exactly one reason: it makes the rest of this decoder checkable against
/// FFmpeg to the last bit. It is a testing convenience and not a claim about
/// the format.
fn lcg_next(state: u32) -> u32 {
    state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223)
}

// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn apply_pns(
    ics: &Ics,
    sfb_cb: &[[u8; MAX_SFB]; WINDOWS],
    sf: &[[i32; MAX_SFB]; WINDOWS],
    rng: &mut u32,
    out: &mut [f32; FRAME_LEN],
) {
    let win_len = ics.win_len();
    let mut group_start = 0usize;

    for g in 0..ics.num_window_groups {
        for sb in 0..ics.max_sfb {
            if sfb_cb[g][sb] != CB_NOISE {
                continue;
            }
            let lo = usize::from(ics.swb_offset[sb]);
            let hi = usize::from(ics.swb_offset[sb + 1]);
            // Not the same exponent as a normal band. A normal scalefactor is
            // a gain, 2^((sf-100)/4); a noise scalefactor names the band's total
            // *energy*, 2^(sf/2), so the amplitude the band is normalised to is
            // its square root.
            let gain = 2.0f32.powf(sf[g][sb] as f32 * 0.25);
            for w in 0..ics.group_len[g] {
                let base = (group_start + w) * win_len;
                let mut energy = 0.0f64;
                for k in lo..hi {
                    if base + k >= FRAME_LEN {
                        break;
                    }
                    *rng = lcg_next(*rng);
                    let v = (*rng as i32) as f32;
                    out[base + k] = v;
                    energy += f64::from(v) * f64::from(v);
                }
                if energy <= 0.0 {
                    continue;
                }
                let scale = (f64::from(gain) / energy.sqrt()) as f32;
                for k in lo..hi {
                    if base + k >= FRAME_LEN {
                        break;
                    }
                    out[base + k] *= scale;
                }
            }
        }
        group_start += ics.group_len[g];
    }
}

/// x = sign(q) * |q|^(4/3) * 2^((sf - 100) / 4)
// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn dequantise(
    ics: &Ics,
    sfb_cb: &[[u8; MAX_SFB]; WINDOWS],
    sf: &[[i32; MAX_SFB]; WINDOWS],
    quant: &[i32; FRAME_LEN],
    out: &mut [f32; FRAME_LEN],
) {
    out.fill(0.0);
    let win_len = ics.win_len();
    let mut group_start = 0usize;

    for g in 0..ics.num_window_groups {
        for s in 0..ics.max_sfb {
            let cb = sfb_cb[g][s];
            if cb == CB_ZERO
                || cb == CB_NOISE
                || cb == CB_INTENSITY
                || cb == CB_INTENSITY2
                || cb > 11
            {
                continue;
            }
            let gain = 2.0f32.powf((sf[g][s] - 100) as f32 * 0.25);
            let lo = usize::from(ics.swb_offset[s]);
            let hi = usize::from(ics.swb_offset[s + 1]);
            for w in 0..ics.group_len[g] {
                let base = (group_start + w) * win_len;
                for k in lo..hi {
                    if base + k >= FRAME_LEN {
                        break;
                    }
                    let q = quant[base + k];
                    if q == 0 {
                        continue;
                    }
                    let magnitude = (q.unsigned_abs() as f32).powf(4.0 / 3.0);
                    out[base + k] = if q < 0 { -magnitude } else { magnitude } * gain;
                }
            }
        }
        group_start += ics.group_len[g];
    }
}

// ------------------------------------------------------------- joint stereo

/// Mid/side and intensity, both of which turn one channel's spectrum into two.
///
/// They are disjoint by construction: intensity is signalled by the *right*
/// channel's codebook being 14 or 15, and a band coded that way is never also
/// M/S. So the two loops below cannot both touch the same band, whatever order
/// they run in.
// The standard's loops: one band at a time across parallel arrays, which
// is how the text reads and how it is checked against the text.
#[allow(clippy::needless_range_loop)]
fn apply_joint_stereo(st: &mut State, left_index: usize, right_index: usize) {
    let (head, tail) = st.chan.split_at_mut(right_index);
    let left = &mut head[left_index];
    let right = &mut tail[0];
    let ms_used = &st.ms_used[left_index];
    let ics = left.ics;
    let win_len = ics.win_len();

    let mut group_start = 0usize;
    for g in 0..ics.num_window_groups {
        for sb in 0..ics.max_sfb {
            let cb = right.sfb_cb[g][sb];
            let lo = usize::from(ics.swb_offset[sb]);
            let hi = usize::from(ics.swb_offset[sb + 1]);

            if cb == CB_INTENSITY || cb == CB_INTENSITY2 {
                // The right channel is a scaled copy of the left. cb 15 keeps
                // the sign, cb 14 inverts it, and M/S on the band inverts it
                // again.
                let mut sign = if cb == CB_INTENSITY { 1.0f32 } else { -1.0f32 };
                if ms_used[g][sb] {
                    sign = -sign;
                }
                let scale = sign * 0.5f32.powf(right.sf[g][sb] as f32 * 0.25);
                for w in 0..ics.group_len[g] {
                    let base = (group_start + w) * win_len;
                    for k in lo..hi {
                        if base + k >= FRAME_LEN {
                            break;
                        }
                        right.coeffs[base + k] = left.coeffs[base + k] * scale;
                    }
                }
                continue;
            }

            // M/S needs two ordinary bands. Where either channel substituted
            // noise for this band there is no side signal to add and subtract,
            // and a band the standard leaves alone is one this must leave alone
            // too: doing otherwise turns the left channel's noise into a
            // difference nobody encoded.
            if !ms_used[g][sb] || cb >= CB_NOISE || left.sfb_cb[g][sb] >= CB_NOISE {
                continue;
            }
            for w in 0..ics.group_len[g] {
                let base = (group_start + w) * win_len;
                for k in lo..hi {
                    if base + k >= FRAME_LEN {
                        break;
                    }
                    let mid = left.coeffs[base + k];
                    let side = right.coeffs[base + k];
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
fn filterbank(
    t: &Tables,
    ics: &Ics,
    coeffs: &[f32; FRAME_LEN],
    overlap: &mut [f32; FRAME_LEN],
    prev_shape: u32,
    out: &mut [f32; FRAME_LEN],
) {
    let mut z = [0.0f32; 2 * FRAME_LEN];
    let w_prev_long = t.window(prev_shape, false);
    let w_cur_long = t.window(ics.window_shape, false);
    let w_prev_short = t.window(prev_shape, true);
    let w_cur_short = t.window(ics.window_shape, true);

    if ics.window_sequence == EIGHT_SHORT {
        let mut tmp = [0.0f32; 2 * SHORT_LEN];
        for w in 0..WINDOWS {
            imdct(
                &t.cos_short,
                &coeffs[w * SHORT_LEN..(w + 1) * SHORT_LEN],
                &mut tmp,
            );
            // Window w's rising half meets window w-1's falling half, so it uses
            // the previous *window's* shape -- which for the first is the
            // previous frame's.
            let rise = if w == 0 { w_prev_short } else { w_cur_short };
            for n in 0..SHORT_LEN {
                tmp[n] *= rise[n];
                tmp[SHORT_LEN + n] *= w_cur_short[SHORT_LEN - 1 - n];
            }
            let at = 448 + w * SHORT_LEN;
            for n in 0..2 * SHORT_LEN {
                z[at + n] += tmp[n];
            }
        }
    } else {
        imdct(&t.cos_long, coeffs, &mut z);
        match ics.window_sequence {
            LONG_START => {
                for n in 0..FRAME_LEN {
                    z[n] *= w_prev_long[n];
                }
                // 1024..1471 pass through, 1472..1599 fall on a short window,
                // 1600..2047 are zero: this is the frame that hands over to
                // eight short ones.
                for n in 0..SHORT_LEN {
                    z[1472 + n] *= w_cur_short[SHORT_LEN - 1 - n];
                }
                for v in &mut z[1600..2 * FRAME_LEN] {
                    *v = 0.0;
                }
            }
            LONG_STOP => {
                for v in &mut z[..448] {
                    *v = 0.0;
                }
                for n in 0..SHORT_LEN {
                    z[448 + n] *= w_prev_short[n];
                }
                // 576..1023 pass through.
                for n in 0..FRAME_LEN {
                    z[FRAME_LEN + n] *= w_cur_long[FRAME_LEN - 1 - n];
                }
            }
            _ => {
                // ONLY_LONG
                for n in 0..FRAME_LEN {
                    z[n] *= w_prev_long[n];
                    z[FRAME_LEN + n] *= w_cur_long[FRAME_LEN - 1 - n];
                }
            }
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
    const FULL_SCALE: f32 = 1.0 / 32768.0;

    for n in 0..FRAME_LEN {
        out[n] = (z[n] + overlap[n]) * FULL_SCALE;
        overlap[n] = z[FRAME_LEN + n];
    }
}

// ----------------------------------------------------------------- decoder

/// The decoder: a configuration, the lapped-transform state that makes AAC
/// frames depend on each other, and the noise generator.
pub struct Decoder {
    tables: &'static Tables,
    cfg: Config,
    channels: usize,
    bits_used: usize,
    bits_given: usize,
    /// The second half of the previous frame's windowed output, per channel.
    overlap: Box<[[f32; FRAME_LEN]; MAX_CHANNELS]>,
    prev_shape: [u32; MAX_CHANNELS],
    pcm: Box<[[f32; FRAME_LEN]; MAX_CHANNELS]>,
    last_sequence: [u32; MAX_CHANNELS],
    layout: ChannelLayout,
    /// The noise generator for PNS bands. Its state runs across frames, so a
    /// decode is reproducible from the start of a track and not from anywhere
    /// else -- which is true of AAC anyway, because the transform is lapped.
    rng: u32,
    state: Box<State>,
}

/// Reads one individual_channel_stream into channel `index` of `st`, then
/// dequantises it and fills its noise bands. `common` is the ics_info a
/// channel pair shares, when it does.
#[allow(clippy::too_many_arguments)]
fn read_ics(
    r: &mut BitReader,
    t: &Tables,
    cfg: &Config,
    rng: &mut u32,
    st: &mut State,
    index: usize,
    common: Option<&Ics>,
) -> Result<(), Error> {
    let State { chan, quant, .. } = st;
    let ch = &mut chan[index];
    ch.tns = NO_TNS;
    let global_gain = r.read(8) as i32;
    match common {
        None => {
            if !read_ics_info(r, cfg, &mut ch.ics) {
                return Err(Error("individual_channel_stream: bad ics_info"));
            }
        }
        Some(shared) => ch.ics = *shared,
    }
    ch.sfb_cb = [[0; MAX_SFB]; WINDOWS];
    if !read_section_data(r, &ch.ics, &mut ch.sfb_cb) {
        return Err(Error("individual_channel_stream: bad section_data"));
    }
    if !read_scale_factors(r, t, &ch.ics, &ch.sfb_cb, global_gain, &mut ch.sf) {
        return Err(Error("individual_channel_stream: bad scale_factor_data"));
    }
    if r.bit() != 0 {
        // pulse_data_present
        if ch.ics.is_short() || !read_pulse_data(r, &ch.ics) {
            return Err(Error("individual_channel_stream: bad pulse_data"));
        }
    }
    if r.bit() != 0 {
        // tns_data_present
        if !read_tns_data(r, &ch.ics, &mut ch.tns) {
            return Err(Error("individual_channel_stream: bad tns_data"));
        }
    }
    if r.bit() != 0 {
        // gain_control_data_present: SSR only
        return Err(Error("gain control is not part of AAC-LC"));
    }
    if !read_spectral_data(r, t, &ch.ics, &ch.sfb_cb, quant) {
        return Err(Error("individual_channel_stream: bad spectral_data"));
    }

    dequantise(&ch.ics, &ch.sfb_cb, &ch.sf, quant, &mut ch.coeffs);
    apply_pns(&ch.ics, &ch.sfb_cb, &ch.sf, rng, &mut ch.coeffs);
    Ok(())
}

impl Decoder {
    pub fn new(cfg: &Config) -> Result<Decoder, Error> {
        let tables = tables()?;
        if cfg.object_type != 2 || cfg.sample_rate == 0 || cfg.rate_index >= 13 {
            return Err(Error("not an AAC-LC configuration"));
        }
        if cfg.frame_960 {
            return Err(Error("960-sample frames are not implemented"));
        }
        let layout = if cfg.channel_config != 0 {
            layout_for_config(cfg.channel_config)
        } else {
            cfg.pce
        };
        Ok(Decoder {
            tables,
            cfg: *cfg,
            channels: 0,
            bits_used: 0,
            bits_given: 0,
            overlap: Box::new([[0.0; FRAME_LEN]; MAX_CHANNELS]),
            prev_shape: [0; MAX_CHANNELS],
            pcm: Box::new([[0.0; FRAME_LEN]; MAX_CHANNELS]),
            last_sequence: [0; MAX_CHANNELS],
            layout,
            rng: NOISE_SEED,
            state: State::new(),
        })
    }

    /// Back to the state a fresh `new` gives: no overlap, sine shapes, and the
    /// noise generator at its seed.
    ///
    /// The generator is part of the decoder's state, not a global source of
    /// entropy: two decodes of the same file have to produce the same noise,
    /// and a seek back to the start has to produce the same noise as an open.
    /// Leaving this out cost a day, in the C++ this is a port of: the host
    /// read one frame to prove the decoder worked and then sought back, so
    /// every file whose length was known was decoded with the generator two
    /// frames out of step, and every band the encoder filled with noise came
    /// out different.
    pub fn reset(&mut self) {
        for o in self.overlap.iter_mut() {
            o.fill(0.0);
        }
        self.prev_shape = [0; MAX_CHANNELS];
        self.rng = NOISE_SEED;
        self.channels = 0;
    }

    /// Decodes one raw_data_block into `FRAME_LEN` samples per channel.
    ///
    /// On success `channels()` is how many channels the frame carried and
    /// `pcm(i)` is each one's output, in the order the elements appeared.
    pub fn decode_frame(&mut self, packet: &[u8]) -> Result<(), Error> {
        self.channels = 0;
        let t = self.tables;
        let mut r = BitReader::new(packet);
        self.bits_given = r.size();

        let st = &mut *self.state;
        st.pair_of = [-1; MAX_CHANNELS];

        loop {
            if r.left() < 3 {
                return Err(Error("the packet ended before the frame did"));
            }
            let id = r.read(3);
            if id == ID_END {
                break;
            }

            match id {
                ID_SCE | ID_LFE => {
                    if self.channels >= MAX_CHANNELS {
                        return Err(Error("more channels than this decoder holds"));
                    }
                    r.read(4); // element_instance_tag
                    read_ics(&mut r, t, &self.cfg, &mut self.rng, st, self.channels, None)?;
                    self.channels += 1;
                }
                ID_CPE => {
                    if self.channels + 2 > MAX_CHANNELS {
                        return Err(Error("more channels than this decoder holds"));
                    }
                    r.read(4); // element_instance_tag
                    let mut common = NO_ICS;
                    let common_window = r.bit() != 0;
                    if common_window {
                        if !read_ics_info(&mut r, &self.cfg, &mut common) {
                            return Err(Error("channel_pair_element: bad ics_info"));
                        }
                        let ms_mask_present = r.read(2);
                        if ms_mask_present == 3 {
                            return Err(Error("channel_pair_element: reserved ms_mask_present"));
                        }
                        for g in 0..common.num_window_groups {
                            for sb in 0..common.max_sfb {
                                let mut used = ms_mask_present == 2;
                                if ms_mask_present == 1 {
                                    used = r.bit() != 0;
                                }
                                st.ms_used[self.channels][g][sb] = used;
                            }
                        }
                    }
                    let shared = if common_window { Some(&common) } else { None };
                    read_ics(
                        &mut r,
                        t,
                        &self.cfg,
                        &mut self.rng,
                        st,
                        self.channels,
                        shared,
                    )?;
                    read_ics(
                        &mut r,
                        t,
                        &self.cfg,
                        &mut self.rng,
                        st,
                        self.channels + 1,
                        shared,
                    )?;
                    st.pair_of[self.channels] = (self.channels + 1) as i32;
                    self.channels += 2;
                }
                ID_DSE => {
                    r.read(4); // element_instance_tag
                    let align = r.bit() != 0;
                    let mut count = r.read(8) as usize;
                    if count == 255 {
                        count += r.read(8) as usize;
                    }
                    if align {
                        r.byte_align();
                    }
                    r.skip(count * 8);
                }
                ID_FIL => {
                    let mut count = r.read(4) as usize;
                    if count == 15 {
                        count += r.read(8) as usize;
                        count -= 1;
                    }
                    r.skip(count * 8);
                }
                ID_PCE => {
                    // A raw ADTS stream has no AudioSpecificConfig, so when its
                    // header says configuration 0 the layout arrives here, once
                    // per frame.
                    let from_frame = read_pce(&mut r);
                    if self.cfg.channel_config == 0 && from_frame.count != 0 {
                        self.layout = from_frame;
                    }
                }
                _ => {
                    // ID_CCE, and anything the three bits could spell besides
                    return Err(Error("coupling channel elements are not implemented"));
                }
            }

            if r.overrun() {
                return Err(Error("an element ran past the end of the packet"));
            }
        }

        r.byte_align();
        self.bits_used = r.pos();

        if r.overrun() {
            return Err(Error("the frame ran past the end of the packet"));
        }
        if self.channels == 0 {
            return Err(Error("the frame carried no channels"));
        }

        // Joint stereo, then TNS, then the filterbank -- the standard's order,
        // and not interchangeable: M/S and intensity work on the quantised
        // spectrum's scale, TNS shapes what comes out of that, and only then is
        // it a signal.
        for c in 0..self.channels {
            if st.pair_of[c] >= 0 {
                apply_joint_stereo(st, c, st.pair_of[c] as usize);
            }
        }
        let rate_index = self.cfg.rate_index as usize;
        for c in 0..self.channels {
            let ch = &mut st.chan[c];
            apply_tns(&ch.ics, &ch.tns, rate_index, &mut ch.coeffs);
            filterbank(
                t,
                &ch.ics,
                &ch.coeffs,
                &mut self.overlap[c],
                self.prev_shape[c],
                &mut self.pcm[c],
            );
            self.last_sequence[c] = ch.ics.window_sequence;
            self.prev_shape[c] = ch.ics.window_shape;
        }
        Ok(())
    }

    /// How many channels the last frame carried.
    pub fn channels(&self) -> usize {
        self.channels
    }

    /// One channel's output from the last frame, in element order.
    pub fn pcm(&self, channel: usize) -> &[f32; FRAME_LEN] {
        &self.pcm[channel]
    }

    /// The layout of the channels `pcm()` hands back.
    ///
    /// For channel configurations 1 to 12 this is a table. For configuration 0
    /// it is whatever the program config element said -- from the
    /// AudioSpecificConfig if the file had one, which an MP4 always does, and
    /// otherwise from the first frame that carries a PCE, which is how a raw
    /// ADTS stream delivers it. That is why this is a method on the decoder
    /// rather than a free function taking the configuration number.
    /// FFmpeg's encoder writes configuration 0 for 7.1(wide), so this is not a
    /// corner nobody reaches.
    pub fn layout(&self) -> &ChannelLayout {
        &self.layout
    }

    /// The window sequence the last frame used for a channel: 0 ONLY_LONG,
    /// 1 LONG_START, 2 EIGHT_SHORT, 3 LONG_STOP. Diagnostic, and the first
    /// thing to look at when one frame in a file is wrong and the rest are not.
    pub fn window_sequence(&self, channel: usize) -> u32 {
        self.last_sequence[channel]
    }

    /// Bits the last frame consumed, and the bits it was given. A frame that
    /// parsed correctly ends inside the last byte of its packet; a parser that
    /// has gone wrong almost always ends somewhere else, which makes this the
    /// cheapest correctness check there is.
    pub fn last_bits_used(&self) -> usize {
        self.bits_used
    }

    pub fn last_bits_given(&self) -> usize {
        self.bits_given
    }
}

// ------------------------------------------------------------------ tests
//
// The parser's refusals, and the one property its tables must have.
//
// What this decoder gets right on real files is established elsewhere, by
// parsing every packet of six of them and checking that each frame ends exactly
// on its packet boundary -- 585 packets, zero slack -- and by the hashes
// docs/formats.md records, every one of which the port reproduced. What is
// here is the other half: the configurations that must be turned away, the
// fact that the codebooks build at all, and the one frame that proves the
// noise generator starts where it should.

#[cfg(test)]
mod tests {
    use super::*;

    /// A real AudioSpecificConfig: AAC-LC, 48 kHz, stereo.
    /// 00010 0011 0010 000 -> AOT 2, freq index 3, channel config 2, GA config 0
    const LC_STEREO_48K: [u8; 2] = [0x11, 0x90];

    fn asc(aot: u8, rate: u8, channels: u8) -> [u8; 2] {
        // The fields do not fall on byte boundaries -- 5 bits of object type,
        // 4 of rate index, 4 of channel configuration, then the GA config --
        // so the bytes are built rather than poked at.
        [
            (aot << 3) | (rate >> 1),
            ((rate & 1) << 7) | (channels << 3),
        ]
    }

    #[test]
    fn a_real_audiospecificconfig_parses_into_the_values_it_encodes() {
        let cfg = Config::parse(&LC_STEREO_48K).unwrap();
        assert_eq!(cfg.object_type, 2);
        assert_eq!(cfg.sample_rate, 48000);
        assert_eq!(cfg.rate_index, 3);
        assert_eq!(cfg.channel_config, 2);
        assert!(!cfg.frame_960);
    }

    #[test]
    fn the_standards_thirteen_sample_rates_are_the_ones_this_decoder_knows() {
        assert_eq!(rate_for_index(0), 96000);
        assert_eq!(rate_for_index(3), 48000);
        assert_eq!(rate_for_index(4), 44100);
        assert_eq!(rate_for_index(11), 8000);
        assert_eq!(rate_for_index(12), 7350);
        assert_eq!(rate_for_index(13), 0);
        assert_eq!(rate_for_index(255), 0);
    }

    #[test]
    fn too_short_to_be_a_config() {
        assert!(Config::parse(&LC_STEREO_48K[..1]).is_err());
        assert!(Config::parse(&[]).is_err());
    }

    #[test]
    fn he_aac_and_he_aacv2_are_refused_not_decoded_at_half_the_rate() {
        // Only the core would decode, at half the sample rate, and sounding
        // like it. Refusing sends the file to demux_ffmpeg instead.
        for aot in [5u8, 29] {
            assert!(Config::parse(&asc(aot, 3, 2)).is_err(), "object type {aot}");
        }
    }

    #[test]
    fn object_types_that_are_not_aac_lc_at_all() {
        for aot in [1u8, 3, 4, 23] {
            assert!(Config::parse(&asc(aot, 3, 2)).is_err(), "object type {aot}");
        }
    }

    #[test]
    fn a_channel_configuration_with_no_layout_is_refused() {
        for channels in [8u8, 9, 10, 15] {
            assert!(
                Config::parse(&asc(2, 3, channels)).is_err(),
                "configuration {channels}"
            );
        }
    }

    #[test]
    fn seven_point_one_is_configuration_12_and_is_accepted() {
        let cfg = Config::parse(&asc(2, 3, 12)).unwrap();
        assert_eq!(cfg.channel_config, 12);
        assert_eq!(layout_for_config(12).count, 8);
    }

    #[test]
    fn configuration_0_takes_its_layout_from_the_program_config_element() {
        // FFmpeg writes configuration 0 for 7.1(wide), putting the layout in a
        // program_config_element inside the AudioSpecificConfig rather than in
        // the configuration field. This is that config, taken from a real file:
        // one front SCE, two front CPEs, one back CPE and an LFE.
        let asc: [u8; 26] = [
            0x11, 0x80, 0x04, 0xCC, 0x05, 0x00, 0x01, 0x08, 0xC8, 0x00, 0x0C, 0x4C, 0x61, 0x76,
            0x63, 0x36, 0x33, 0x2E, 0x31, 0x2E, 0x31, 0x30, 0x31, 0x56, 0xE5, 0x00,
        ];
        let cfg = Config::parse(&asc).unwrap();
        assert_eq!(cfg.channel_config, 0);
        assert_eq!(cfg.sample_rate, 48000);

        assert_eq!(cfg.pce.count, 8);
        assert_eq!(cfg.pce.mask, 0xFF); // FL FR FC LFE BL BR FLC FRC

        // The elements arrive as C, then the two front pairs, then the back
        // pair, then the LFE -- and front pairs are listed from the centre
        // outwards, so the first pair is front-left-of-centre and the second is
        // front left. Reading those the other way round decodes every channel
        // perfectly and still puts two of them in the wrong speakers.
        assert_eq!(cfg.pce.from, [3, 4, 0, 7, 5, 6, 1, 2]);
    }

    #[test]
    fn configuration_0_without_a_program_config_element_is_refused() {
        // Nothing in the file says what the channels are, so there is nothing
        // to guess from. This is the same two bytes as the stereo config with
        // the configuration field zeroed.
        assert!(Config::parse(&[0x11, 0x80]).is_err());
    }

    #[test]
    fn every_layout_is_a_permutation() {
        for config in [1u32, 2, 3, 4, 5, 6, 7, 11, 12] {
            let layout = layout_for_config(config);
            let n = usize::from(layout.count);
            assert!(n > 0, "configuration {config}");
            let mut seen = [false; 8];
            for &from in &layout.from[..n] {
                assert!(
                    !seen[usize::from(from)],
                    "configuration {config} uses a channel twice"
                );
                seen[usize::from(from)] = true;
            }
            assert_eq!(seen.iter().filter(|&&s| s).count(), n);
        }
    }

    /// The first frame of a 48 kHz mono stream at 64 kbit/s, which substitutes
    /// noise in eight of its bands.
    const NOISY_FRAME: [u8; 260] = [
        0xDC, 0x00, 0x4C, 0x61, 0x76, 0x63, 0x36, 0x33, 0x2E, 0x31, 0x2E, 0x31, 0x30, 0x31, 0x00,
        0x02, 0x8C, 0xA9, 0x52, 0xE1, 0xBA, 0x18, 0x16, 0x86, 0x8B, 0xA2, 0x81, 0x69, 0x17, 0x7E,
        0xB5, 0xCF, 0x9A, 0xAC, 0xF8, 0xDE, 0x71, 0x7C, 0xF3, 0xED, 0x4D, 0x21, 0xF8, 0xBA, 0xA9,
        0x15, 0x22, 0xB2, 0x48, 0x2B, 0x5A, 0xE5, 0x9F, 0x09, 0x68, 0x44, 0x31, 0x09, 0x3D, 0x10,
        0xA3, 0x1D, 0xC9, 0xBF, 0xBA, 0x30, 0x89, 0xBC, 0x7E, 0xE9, 0xC2, 0x18, 0x20, 0xF4, 0x32,
        0x26, 0x38, 0x0A, 0x89, 0x57, 0x71, 0x63, 0xB6, 0xF0, 0x05, 0xF2, 0x13, 0x03, 0xCD, 0xA9,
        0x67, 0x71, 0xC0, 0xA6, 0x18, 0x33, 0x6D, 0x9D, 0x80, 0x64, 0x44, 0x91, 0x79, 0xF2, 0x1B,
        0x3D, 0xDF, 0x0F, 0xBB, 0xB7, 0x86, 0xDF, 0xB0, 0xD0, 0x55, 0x8C, 0xEB, 0x75, 0x39, 0x54,
        0x6B, 0xB8, 0xB7, 0x74, 0x16, 0xC6, 0x5B, 0x56, 0x7C, 0x64, 0xD5, 0xB7, 0x57, 0x7F, 0xBB,
        0x8C, 0x8D, 0x2D, 0x06, 0xEF, 0xC8, 0xA9, 0x28, 0xD4, 0x19, 0xB7, 0x3F, 0x91, 0x8B, 0xE1,
        0x9D, 0xBA, 0x76, 0xF9, 0x04, 0xE0, 0x5C, 0xA0, 0x02, 0x01, 0xAF, 0xD3, 0x83, 0x26, 0x54,
        0xD8, 0xF3, 0xDE, 0xF1, 0x6F, 0xF1, 0x61, 0xF1, 0xF1, 0x04, 0xD0, 0xF9, 0x0B, 0x7E, 0x3E,
        0x4D, 0xFD, 0x3B, 0xF8, 0xEB, 0xDD, 0xAA, 0xEC, 0x14, 0x86, 0x2A, 0x76, 0x39, 0x38, 0xFC,
        0x3F, 0x5B, 0xB7, 0x5E, 0x31, 0x66, 0xA4, 0x2B, 0x03, 0x88, 0x8B, 0xDE, 0x3D, 0xD0, 0xF5,
        0xC7, 0xC2, 0x7C, 0x1E, 0xF6, 0x5B, 0xC4, 0xA9, 0x86, 0x55, 0x42, 0x37, 0x09, 0xE2, 0x31,
        0xE3, 0x18, 0x18, 0x03, 0xC0, 0x2E, 0xAD, 0x2D, 0x87, 0xD8, 0xB4, 0xD8, 0x56, 0x36, 0x3B,
        0x8D, 0x65, 0x87, 0x81, 0x77, 0x1C, 0x6A, 0x5C, 0x3F, 0x3E, 0x36, 0xB1, 0x57, 0xA7, 0x9A,
        0xAE, 0x41, 0x0C, 0x23, 0xC0,
    ];

    /// AAC-LC, 48 kHz, mono: the configuration NOISY_FRAME was encoded under.
    const LC_MONO_48K: [u8; 2] = [0x11, 0x88];

    #[test]
    fn a_fresh_decoder_and_a_reset_one_decode_a_noisy_frame_identically() {
        // Noise-substituted bands are filled from a generator carried in the
        // decoder. If a reset leaves it where the last decode stopped, the
        // second decode of a file differs from the first. That is not
        // hypothetical: the host used to read a frame to prove a decoder
        // worked and then seek back, so *every* file whose length was known
        // was decoded from a generator two frames out of step. The bands were
        // the right size and the right energy and held the wrong noise.
        let cfg = Config::parse(&LC_MONO_48K).unwrap();
        let mut decoder = Decoder::new(&cfg).unwrap();
        decoder.decode_frame(&NOISY_FRAME).unwrap();
        assert_eq!(decoder.channels(), 1);
        let first = *decoder.pcm(0);

        decoder.reset();
        decoder.decode_frame(&NOISY_FRAME).unwrap();
        assert!(
            first == *decoder.pcm(0),
            "a reset decoder produced different samples"
        );

        let mut again = Decoder::new(&cfg).unwrap();
        again.decode_frame(&NOISY_FRAME).unwrap();
        assert!(
            first == *again.pcm(0),
            "a fresh decoder produced different samples"
        );

        // And the frame is not silence, so the comparisons above meant something.
        let peak = first.iter().fold(0.0f32, |p, &v| p.max(v.abs()));
        assert!(peak > 0.0);
    }

    #[test]
    fn a_real_frame_ends_inside_the_last_byte_of_its_packet() {
        // The cheapest correctness check there is: a single misread bit
        // desynchronises a frame, and it cannot land on the boundary by luck.
        let cfg = Config::parse(&LC_MONO_48K).unwrap();
        let mut decoder = Decoder::new(&cfg).unwrap();
        decoder.decode_frame(&NOISY_FRAME).unwrap();
        let slack = decoder.last_bits_given() - decoder.last_bits_used();
        assert!(slack < 8, "{slack} bits of slack");
    }

    #[test]
    fn the_codebooks_build_which_is_the_only_thing_a_table_can_promise() {
        // `new` builds the twelve Huffman tries and fails if any codeword is a
        // prefix of another. The generator has already checked the Kraft
        // equality; this checks that the codewords themselves form a usable
        // code, which is a different claim and the one that decoding depends on.
        let cfg = Config::parse(&LC_STEREO_48K).unwrap();
        assert!(Decoder::new(&cfg).is_ok());
    }

    #[test]
    fn nine_hundred_and_sixty_sample_frames_are_refused() {
        let cfg = Config {
            frame_960: true,
            ..Config::parse(&LC_STEREO_48K).unwrap()
        };
        assert!(Decoder::new(&cfg).is_err());
    }

    #[test]
    fn an_empty_packet_is_refused() {
        let cfg = Config::parse(&LC_STEREO_48K).unwrap();
        let mut decoder = Decoder::new(&cfg).unwrap();
        assert!(decoder.decode_frame(&[]).is_err());
    }

    #[test]
    fn bytes_that_are_not_a_frame_are_refused_rather_than_acted_on() {
        let cfg = Config::parse(&LC_STEREO_48K).unwrap();
        let mut decoder = Decoder::new(&cfg).unwrap();
        let junk: Vec<u8> = (0..512u32).map(|i| (i * 61 + 7) as u8).collect();
        // The only requirement is that it returns. A hostile packet is allowed
        // to look decodable, and this one may well parse -- what it must not do
        // is spin, which is what the first version of read_section_data did,
        // or index past a buffer, which is what the language forbids.
        let _ = decoder.decode_frame(&junk);
    }

    #[test]
    fn the_bit_reader_pads_with_zeroes_and_notices_consumption_past_the_end() {
        let mut r = BitReader::new(&[0xA5]);
        assert_eq!(r.read(4), 0xA);
        assert_eq!(r.read(4), 0x5);
        assert!(!r.overrun());
        assert_eq!(r.read(8), 0);
        assert!(r.overrun());
        assert_eq!(r.left(), 0);
    }
}

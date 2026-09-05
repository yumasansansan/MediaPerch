// SPDX-License-Identifier: GPL-3.0-or-later
//
// ALAC, decoded here rather than by somebody else's library, and in safe Rust
// rather than in C++.
//
// The reasoning is in §7 of docs/plan.md and it is short: ALAC's reference
// implementation is the specification *and* has been unmaintained since 2011,
// and those two facts point in opposite directions. Apple's code is where the
// format is defined, and it is also the code that gave Qualcomm and MediaTek
// CVE-2021-30351 and friends when they shipped it. Reading it to learn the
// format is right; linking it is not.
//
// This is a port of the C++ decoder that used to be `alac.cpp`, itself written
// from that reference read as a specification, and it keeps that file's
// checks: every value that comes out of the bitstream is validated before it
// is used as a length or a shift, and the buffers are sized once from a config
// that was checked first. What changes is what happens when a check is missed.
// In C++ a read past a buffer is a read past a buffer; here it is a panic,
// which `mp-abi` turns into an error at the module boundary. The five things
// Apple's decoder does not check are still named in comments below, because
// they are still the five places a crafted file goes.
//
// **`#![forbid(unsafe_code)]`**, and the compiler enforces it. No I/O, no OS,
// no allocation during decode, no dependency: this file is the codec and
// nothing else, which is what lets the tests and the fuzz target drive it
// directly.
//
// **Arithmetic is wrapping wherever the C++ relied on it.** ALAC's adaptive
// Golomb parameters are tuned around unsigned 32-bit wraparound, and the
// predictor's sums overflow on hostile input in ways the reference never
// thought about. `wrapping_*` says so at each site instead of letting a debug
// build panic where a release build would not, and it is what keeps the
// decoded bytes identical to the C++ decoder's on every file in the corpus.

#![forbid(unsafe_code)]

use std::fmt;

/// The largest frame this decoder will size buffers for. ALAC's own default is
/// 4096 and its encoder has no way to ask for more; a cookie claiming tens of
/// millions is a cookie asking us to allocate on its say-so.
pub const MAX_FRAME_LENGTH: u32 = 65536;
pub const MAX_CHANNELS: u8 = 8;

/// Why a cookie or a packet was refused. Fixed strings, so that saying why
/// costs nothing on the decode path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub &'static str);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for Error {}

// ---------------------------------------------------------------- config

/// The ALACSpecificConfig magic cookie, as it appears in an `alac` box.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Config {
    pub frame_length: u32,
    pub compatible_version: u8,
    pub bit_depth: u8,
    pub pb: u8,
    pub mb: u8,
    pub kb: u8,
    pub channels: u8,
    pub max_run: u16,
    pub max_frame_bytes: u32,
    pub avg_bit_rate: u32,
    pub sample_rate: u32,
}

impl Config {
    /// The cookie is 24 bytes big-endian. Anything shorter, or describing a
    /// stream this decoder will not touch, is refused here rather than deeper
    /// in.
    pub fn parse(cookie: &[u8]) -> Result<Config, Error> {
        if cookie.len() < 24 {
            return Err(Error("the cookie is shorter than an ALACSpecificConfig"));
        }
        let be32 = |at: usize| {
            u32::from_be_bytes([cookie[at], cookie[at + 1], cookie[at + 2], cookie[at + 3]])
        };
        let cfg = Config {
            frame_length: be32(0),
            compatible_version: cookie[4],
            bit_depth: cookie[5],
            pb: cookie[6],
            mb: cookie[7],
            kb: cookie[8],
            channels: cookie[9],
            max_run: u16::from_be_bytes([cookie[10], cookie[11]]),
            max_frame_bytes: be32(12),
            avg_bit_rate: be32(16),
            sample_rate: be32(20),
        };
        if cfg.compatible_version != 0 {
            return Err(Error("a compatible-version this decoder has never seen"));
        }
        if !matches!(cfg.bit_depth, 16 | 20 | 24 | 32) {
            return Err(Error("a bit depth ALAC does not define"));
        }
        if cfg.channels == 0 || cfg.channels > MAX_CHANNELS {
            return Err(Error("channel count out of range"));
        }
        if cfg.frame_length == 0 || cfg.frame_length > MAX_FRAME_LENGTH {
            return Err(Error("frame length out of range"));
        }
        if cfg.sample_rate == 0 {
            return Err(Error("a sample rate of zero"));
        }
        Ok(cfg)
    }
}

// ---------------------------------------------------------------- layouts

/// Channels in the order ALAC stores them, which is Apple's and not WAVE's.
///
/// This is the table Media Foundation forgets to apply -- see docs/formats.md.
#[derive(Clone, Copy, Debug)]
pub struct ChannelLayout {
    /// MP_SPEAKER_* bits, or 0 for mono and stereo like every other decoder.
    pub mask: u32,
    /// For each WAVE slot, the ALAC channel that fills it.
    pub from: [u8; 8],
}

/// ALAC's channel order, from Apple's own table, mapped onto WAVE slots.
///
/// 1: C            2: L R          3: C L R
/// 4: C L R Cs     5: C L R Ls Rs  6: C L R Ls Rs LFE
/// 7: C L R Ls Rs Cs LFE           8: C Lc Rc L R Ls Rs LFE
///
/// `from[slot]` is the ALAC channel that belongs in that WAVE slot, with the
/// slots in ascending SPEAKER_* bit order.
const LAYOUTS: [ChannelLayout; 9] = [
    ChannelLayout {
        mask: 0,
        from: [0, 0, 0, 0, 0, 0, 0, 0],
    },
    ChannelLayout {
        mask: 0,
        from: [0, 0, 0, 0, 0, 0, 0, 0],
    },
    ChannelLayout {
        mask: 0,
        from: [0, 1, 0, 0, 0, 0, 0, 0],
    },
    // 3: FL FR FC
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4,
        from: [1, 2, 0, 0, 0, 0, 0, 0],
    },
    // 4: FL FR FC BC
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4 | 0x100,
        from: [1, 2, 0, 3, 0, 0, 0, 0],
    },
    // 5: FL FR FC BL BR
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4 | 0x10 | 0x20,
        from: [1, 2, 0, 3, 4, 0, 0, 0],
    },
    // 6: FL FR FC LFE BL BR
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20,
        from: [1, 2, 0, 5, 3, 4, 0, 0],
    },
    // 7: FL FR FC LFE BC SL SR
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400,
        from: [1, 2, 0, 6, 5, 3, 4, 0],
    },
    // 8: FL FR FC LFE BL BR FLC FRC
    ChannelLayout {
        mask: 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80,
        from: [3, 4, 0, 7, 5, 6, 1, 2],
    },
];

pub fn layout_for(channels: u8) -> &'static ChannelLayout {
    &LAYOUTS[if channels <= MAX_CHANNELS {
        channels as usize
    } else {
        0
    }]
}

// ------------------------------------------------------------- constants
// These are the format's, and the names are the reference's so that the two
// can be read side by side.

const QBSHIFT: u32 = 9;
const QB: u32 = 1 << QBSHIFT;
const MMULSHIFT: u32 = 2;
const MDENSHIFT: u32 = QBSHIFT - MMULSHIFT - 1; // 6
const MOFF: u32 = 1 << (MDENSHIFT - 2); // 16
const BITOFF: i32 = 24;
const MAX_PREFIX_16: u32 = 9;
const MAX_PREFIX_32: u32 = 9;
const MAX_DATATYPE_BITS_16: u32 = 16;
const MEAN_CLAMP: u32 = 0xffff;

const ID_SCE: u32 = 0;
const ID_CPE: u32 = 1;
const ID_CCE: u32 = 2;
const ID_LFE: u32 = 3;
const ID_DSE: u32 = 4;
const ID_PCE: u32 = 5;
const ID_FIL: u32 = 6;
const ID_END: u32 = 7;

/// Leading zeros, with lead(0) == 32 -- the reference's convention, and the one
/// the adaptive Golomb parameters are tuned around.
#[inline]
fn lead(x: u32) -> u32 {
    x.leading_zeros()
}

#[inline]
fn lg3a(x: u32) -> i32 {
    31 - lead(x.wrapping_add(3)) as i32
}

#[inline]
fn sign_of(v: i32) -> i32 {
    (v >> 31) | ((v.wrapping_neg() as u32) >> 31) as i32
}

/// Sign-extends the low `bits` of `v`. `bits` is always 1..=32 here.
#[inline]
fn extend(v: i32, bits: u32) -> i32 {
    if bits >= 32 {
        return v;
    }
    let shift = 32 - bits;
    (((v as u32) << shift) as i32) >> shift
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
// the last sample of a legitimate packet genuinely peeks past the end -- and
// it is *consuming* past the end that is checked, once, where the answer
// matters.
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

    #[inline]
    fn pos(&self) -> usize {
        self.pos
    }

    fn seek(&mut self, bit: usize) {
        self.pos = bit;
    }

    #[inline]
    fn size(&self) -> usize {
        self.bits
    }

    #[inline]
    fn exhausted(&self) -> bool {
        self.pos > self.bits
    }

    fn has(&self, n: usize) -> bool {
        self.pos <= self.bits && (self.bits - self.pos) >= n
    }

    /// 32 bits starting at `bit`, MSB first, zero-padded past the end.
    ///
    /// Padding rather than failing is deliberate: the last sample of a
    /// perfectly good packet peeks past its end, so a peek is not evidence of
    /// anything. What is checked, and checked where it means something, is
    /// consumption.
    #[inline]
    fn peek32(&self, bit: usize) -> u32 {
        let first = bit >> 3;
        let off = (bit & 7) as u32;
        let mut window: u64 = 0;
        for i in 0..5 {
            let byte = self.data.get(first.saturating_add(i)).copied().unwrap_or(0);
            window = (window << 8) | u64::from(byte);
        }
        // `window` holds bits [first*8, first*8+40); take 32 starting at `off`.
        ((window >> (8 - off)) & 0xffff_ffff) as u32
    }

    /// Reads `n` bits (0..=32) and advances. Past the end this yields zeroes
    /// and leaves `exhausted()` true.
    #[inline]
    fn read(&mut self, n: u32) -> u32 {
        if n == 0 {
            return 0;
        }
        let v = self.peek32(self.pos);
        self.pos += n as usize;
        if n >= 32 {
            v
        } else {
            v >> (32 - n)
        }
    }

    #[inline]
    fn advance(&mut self, n: usize) {
        self.pos += n;
    }

    /// The Golomb readers over-advance by design and then step back one bit.
    #[inline]
    fn retreat(&mut self, n: usize) {
        self.pos = self.pos.saturating_sub(n);
    }

    fn byte_align(&mut self) {
        self.pos = (self.pos + 7) & !7usize;
    }
}

// ------------------------------------------------------- adaptive Golomb

struct AgParams {
    mb0: u32,
    pb: u32,
    kb: u32,
    wb: u32,
}

/// `k` indexes a shift, so it has to be bounded before it is used as one.
///
/// The reference derives k from values the bitstream controls and then writes
/// `(1 << k) - 1` and `x >> (32 - k)` without checking it. Both are undefined
/// for k of 0 or 32 and neither is unreachable from a crafted file.
#[inline]
fn clamp_k(k: i32) -> u32 {
    k.clamp(1, 31) as u32
}

fn dyn_get_32bit(r: &mut BitReader<'_>, m: u32, k: u32, maxbits: u32) -> u32 {
    let stream = r.peek32(r.pos());
    let mut result = lead(!stream);

    if result >= MAX_PREFIX_32 {
        let at = r.pos() + MAX_PREFIX_32 as usize;
        let v = r.peek32(at);
        result = if maxbits >= 32 {
            v
        } else {
            v >> (32 - maxbits)
        };
        r.advance((MAX_PREFIX_32 + maxbits) as usize);
        return result;
    }

    r.advance(result as usize + 1);
    if k != 1 {
        let v = (stream << (result + 1)) >> (32 - k);
        r.advance(k as usize);
        r.retreat(1);
        result = result.wrapping_mul(m);
        if v >= 2 {
            result = result.wrapping_add(v - 1);
            r.advance(1);
        }
    }
    result
}

fn dyn_get(r: &mut BitReader<'_>, m: u32, k: u32) -> u32 {
    let stream = r.peek32(r.pos());
    let mut pre = lead(!stream);

    if pre >= MAX_PREFIX_16 {
        pre = MAX_PREFIX_16;
        r.advance(pre as usize);
        let result = (stream << pre) >> (32 - MAX_DATATYPE_BITS_16);
        r.advance(MAX_DATATYPE_BITS_16 as usize);
        return result;
    }

    r.advance(pre as usize + 1);
    let v = (stream << (pre + 1)) >> (32 - k);
    r.advance(k as usize);
    let mut result = pre.wrapping_mul(m).wrapping_add(v).wrapping_sub(1);
    if v < 2 {
        result = result.wrapping_sub(v.wrapping_sub(1));
        r.retreat(1);
    }
    result
}

/// Decodes `out.len()` residuals. False on a malformed stream.
fn dyn_decomp(p: &AgParams, r: &mut BitReader<'_>, out: &mut [i32], maxbits: u32) -> bool {
    let count = out.len();
    let mut mb = p.mb0;
    let mut zmode: u32 = 0;
    let mut c = 0usize;

    while c < count {
        if r.pos() >= r.size() {
            return false;
        }

        let mut m = mb >> QBSHIFT;
        let mut k = clamp_k(lg3a(m));
        if k > p.kb {
            k = p.kb;
        }
        k = clamp_k(k as i32);
        m = (1u32 << k) - 1;

        let n = dyn_get_32bit(r, m, k, maxbits);

        let ndecode = n.wrapping_add(zmode);
        let multiplier = -((ndecode & 1) as i32) | 1;
        out[c] = ((ndecode.wrapping_add(1) >> 1) as i32).wrapping_mul(multiplier);
        c += 1;

        mb =
            p.pb.wrapping_mul(n.wrapping_add(zmode))
                .wrapping_add(mb)
                .wrapping_sub(p.pb.wrapping_mul(mb) >> QBSHIFT);
        if n > MEAN_CLAMP {
            mb = MEAN_CLAMP;
        }

        zmode = 0;
        if (mb << MMULSHIFT) < QB && c < count {
            zmode = 1;
            let raw = lead(mb) as i32 - BITOFF + (mb.wrapping_add(MOFF) >> MDENSHIFT) as i32;
            let kz = clamp_k(raw);
            let mz = ((1u32 << kz) - 1) & p.wb;
            let run = dyn_get(r, mz, kz) as usize;

            // The reference checks this too, and it is the one place it does.
            if run > count - c {
                return false;
            }
            out[c..c + run].fill(0);
            c += run;
            if run >= 65535 {
                zmode = 0;
            }
            mb = 0;
        }
    }

    !r.exhausted()
}

// --------------------------------------------------------------- predictor

/// The "in place" mode the reference runs before the real predictor when the
/// prediction type is not zero: a running sum, sign-extended at the channel's
/// width. The C++ called `unpc_block` with `in == out` for this; Rust cannot
/// alias a slice with itself, so it is its own function with the same
/// arithmetic.
fn unpc_inplace_31(buf: &mut [i32], chanbits: u32) {
    if buf.is_empty() {
        return;
    }
    let mut prev = buf[0];
    for j in 1..buf.len() {
        let del = buf[j].wrapping_add(prev);
        prev = extend(del, chanbits);
        buf[j] = prev;
    }
}

/// The inverse of the adaptive LPC. `coefs` is updated as it goes, which is the
/// adaptation; it is the caller's copy and is refilled from the stream per
/// element, so nothing persists between frames.
fn unpc_block(
    input: &[i32],
    output: &mut [i32],
    coefs: &mut [i16; 32],
    numactive: usize,
    chanbits: u32,
    denshift: u32,
) {
    let num = input.len().min(output.len());
    if num == 0 {
        return;
    }
    output[0] = input[0];

    if numactive == 0 {
        output[1..num].copy_from_slice(&input[1..num]);
        return;
    }

    if numactive == 31 {
        let mut prev = output[0];
        for j in 1..num {
            let del = input[j].wrapping_add(prev);
            prev = extend(del, chanbits);
            output[j] = prev;
        }
        return;
    }

    // Warm-up: the first `numactive` samples are plain deltas.
    //
    // The reference writes out[1..numactive] without checking that the frame
    // is that long, so a short partial frame with a large coefficient count
    // writes past the end of the buffer. `numactive` is five bits from the
    // stream and `num` can be as small as one.
    let warm = if numactive + 1 < num {
        numactive
    } else {
        num - 1
    };
    for j in 1..=warm {
        let del = input[j].wrapping_add(output[j - 1]);
        output[j] = extend(del, chanbits);
    }
    if warm < numactive {
        return; // nothing left for the predictor proper
    }

    // denshift is four bits from the stream, and the reference computes
    // `1 << (denshift - 1)` unconditionally at function entry -- undefined when
    // the stream says zero, which it is free to.
    let denhalf: i32 = if denshift == 0 {
        0
    } else {
        1 << (denshift - 1)
    };
    let lim = numactive + 1;

    for j in lim..num {
        let top = output[j - lim];
        let mut sum: i32 = 0;
        for k in 0..numactive {
            sum = sum.wrapping_add(
                i32::from(coefs[k]).wrapping_mul(output[j - 1 - k].wrapping_sub(top)),
            );
        }

        let del = input[j];
        let mut del0 = del;
        let sg = sign_of(del);
        let del = del
            .wrapping_add(top)
            .wrapping_add(sum.wrapping_add(denhalf) >> denshift);
        output[j] = extend(del, chanbits);

        if sg > 0 {
            for k in (0..numactive).rev() {
                let dd = top.wrapping_sub(output[j - 1 - k]);
                let sgn = sign_of(dd);
                coefs[k] = (i32::from(coefs[k]).wrapping_sub(sgn)) as i16;
                let weight = (numactive - k) as i32;
                del0 = del0.wrapping_sub(weight.wrapping_mul(sgn.wrapping_mul(dd) >> denshift));
                if del0 <= 0 {
                    break;
                }
            }
        } else if sg < 0 {
            for k in (0..numactive).rev() {
                let dd = top.wrapping_sub(output[j - 1 - k]);
                let sgn = sign_of(dd);
                coefs[k] = (i32::from(coefs[k]).wrapping_add(sgn)) as i16;
                let weight = (numactive - k) as i32;
                del0 = del0.wrapping_sub(
                    weight.wrapping_mul(sgn.wrapping_neg().wrapping_mul(dd) >> denshift),
                );
                if del0 >= 0 {
                    break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------- decoder

pub struct Decoder {
    cfg: Config,
    predictor: Vec<i32>,
    mix_u: Vec<i32>,
    mix_v: Vec<i32>,
    /// Two per frame because a channel pair fills it interleaved. Zeroed here
    /// and zeroed again per frame: the reference leaves it holding the previous
    /// frame's contents and then ORs it into the output when a crafted stream
    /// sets mixres without setting bytesShifted.
    shift: Vec<u16>,
}

impl Decoder {
    /// Sizes every buffer from `cfg`. An error if the config is not one this
    /// decoder will accept, which `Config::parse` already rules out.
    pub fn new(cfg: &Config) -> Result<Decoder, Error> {
        if cfg.channels == 0 || cfg.channels > MAX_CHANNELS {
            return Err(Error("channel count out of range"));
        }
        if cfg.frame_length == 0 || cfg.frame_length > MAX_FRAME_LENGTH {
            return Err(Error("frame length out of range"));
        }
        if !matches!(cfg.bit_depth, 16 | 20 | 24 | 32) {
            return Err(Error("unsupported bit depth"));
        }
        let n = cfg.frame_length as usize;
        Ok(Decoder {
            cfg: *cfg,
            predictor: vec![0; n],
            mix_u: vec![0; n],
            mix_v: vec![0; n],
            shift: vec![0; n * 2],
        })
    }

    pub fn config(&self) -> &Config {
        &self.cfg
    }

    pub fn channels(&self) -> u8 {
        self.cfg.channels
    }

    pub fn bit_depth(&self) -> u8 {
        self.cfg.bit_depth
    }

    pub fn frame_length(&self) -> u32 {
        self.cfg.frame_length
    }

    /// Decodes one packet. `out` receives `channels()` * result samples,
    /// interleaved, sign-extended and right-justified at `bit_depth()`, in
    /// **ALAC channel order**. `out` must have room for
    /// `channels() * frame_length()` samples.
    pub fn decode(&mut self, packet: &[u8], out: &mut [i32]) -> Result<u32, Error> {
        let channels = usize::from(self.cfg.channels);
        let depth = u32::from(self.cfg.bit_depth);
        if out.len() < channels * self.cfg.frame_length as usize {
            return Err(Error("the output buffer is smaller than a frame"));
        }

        let mut r = BitReader::new(packet);
        let mut samples = self.cfg.frame_length;
        let mut produced: u32 = 0;
        let mut channel_index = 0usize;

        let mut coefs_u = [0i16; 32];
        let mut coefs_v = [0i16; 32];

        while channel_index < channels {
            if r.pos() >= r.size() {
                return Err(Error("packet ended before the frame did"));
            }

            let tag = r.read(3);
            match tag {
                ID_END => break,
                ID_CCE | ID_PCE => {
                    return Err(Error(
                        "coupling and program-config elements are not part of ALAC",
                    ))
                }
                ID_DSE => {
                    r.read(4);
                    let align = r.read(1) != 0;
                    let mut count = r.read(8);
                    if count == 255 {
                        count += r.read(8);
                    }
                    if align {
                        r.byte_align();
                    }
                    r.advance(count as usize * 8);
                    if r.exhausted() {
                        return Err(Error("data stream element runs past the packet"));
                    }
                    continue;
                }
                ID_FIL => {
                    let mut count = r.read(4);
                    if count == 15 {
                        count = count.wrapping_add(r.read(8).wrapping_sub(1));
                    }
                    r.advance(count as usize * 8);
                    if r.exhausted() {
                        return Err(Error("fill element runs past the packet"));
                    }
                    continue;
                }
                ID_SCE | ID_CPE | ID_LFE => {}
                _ => unreachable!("a three-bit tag has eight values and all are named"),
            }

            let pair = tag == ID_CPE;
            let width = if pair { 2 } else { 1 };
            if channel_index + width > channels {
                return Err(Error(
                    "the frame describes more channels than the stream has",
                ));
            }

            r.read(4); // element instance tag
            if r.read(12) != 0 {
                return Err(Error("reserved header bits are not zero"));
            }

            let header = r.read(4);
            let partial = (header >> 3) != 0;
            let mut bytes_shifted = (header >> 1) & 0x3;
            let escape = (header & 1) != 0;
            if bytes_shifted == 3 {
                return Err(Error("invalid shift width"));
            }
            // A channel pair codes one extra bit, because the mid/side residual
            // needs the range. The uncompressed path below overrides this for a
            // pair: there is no residual there, so there is no extra bit.
            let mut chan_bits = depth - bytes_shifted * 8 + u32::from(pair);

            if partial {
                let hi = r.read(16);
                let lo = r.read(16);
                samples = (hi << 16) | lo;
                // The buffers were sized from the config, and the config is the
                // only promise about frame length there is. The reference takes
                // this number and writes that many samples.
                if samples == 0 || samples > self.cfg.frame_length {
                    return Err(Error(
                        "partial frame claims more samples than the config allows",
                    ));
                }
            }
            if chan_bits == 0 || chan_bits > 32 {
                return Err(Error("channel bit width out of range"));
            }
            let n = samples as usize;

            let mix_bits: u32;
            let mix_res: i32;
            let mut shift_start = 0usize;

            if !escape {
                mix_bits = r.read(8);
                mix_res = i32::from(r.read(8) as u8 as i8);

                let b = r.read(8);
                let mode_u = b >> 4;
                let den_u = b & 0xf;
                let b = r.read(8);
                let pb_u = b >> 5;
                let num_u = (b & 0x1f) as usize;
                for coef in coefs_u.iter_mut().take(num_u) {
                    *coef = r.read(16) as u16 as i16;
                }

                let (mut mode_v, mut den_v, mut pb_v, mut num_v) = (0u32, 0u32, 0u32, 0usize);
                if pair {
                    let b = r.read(8);
                    mode_v = b >> 4;
                    den_v = b & 0xf;
                    let b = r.read(8);
                    pb_v = b >> 5;
                    num_v = (b & 0x1f) as usize;
                    for coef in coefs_v.iter_mut().take(num_v) {
                        *coef = r.read(16) as u16 as i16;
                    }
                }

                if bytes_shifted != 0 {
                    shift_start = r.pos();
                    r.advance(bytes_shifted as usize * 8 * n * width);
                    if r.exhausted() {
                        return Err(Error("shift buffer runs past the packet"));
                    }
                }

                // `mix_bits` is a shift below, and a byte from the stream. A
                // real encoder writes a small number; 32 or more would be an
                // undefined shift in the reference and a panic here.
                if mix_bits > 31 {
                    return Err(Error("mix shift out of range"));
                }

                let kb = u32::from(self.cfg.kb);
                let mut p = AgParams {
                    mb0: u32::from(self.cfg.mb),
                    kb,
                    wb: if kb >= 32 { u32::MAX } else { (1u32 << kb) - 1 },
                    pb: (u32::from(self.cfg.pb) * pb_u) / 4,
                };
                if !dyn_decomp(&p, &mut r, &mut self.predictor[..n], chan_bits) {
                    return Err(Error("malformed residual stream"));
                }
                if mode_u != 0 {
                    unpc_inplace_31(&mut self.predictor[..n], chan_bits);
                }
                unpc_block(
                    &self.predictor[..n],
                    &mut self.mix_u[..n],
                    &mut coefs_u,
                    num_u,
                    chan_bits,
                    den_u,
                );

                if pair {
                    p.pb = (u32::from(self.cfg.pb) * pb_v) / 4;
                    if !dyn_decomp(&p, &mut r, &mut self.predictor[..n], chan_bits) {
                        return Err(Error("malformed residual stream"));
                    }
                    if mode_v != 0 {
                        unpc_inplace_31(&mut self.predictor[..n], chan_bits);
                    }
                    unpc_block(
                        &self.predictor[..n],
                        &mut self.mix_v[..n],
                        &mut coefs_v,
                        num_v,
                        chan_bits,
                        den_v,
                    );
                }
            } else {
                // Uncompressed: the samples are simply there, at chan_bits each.
                if pair {
                    chan_bits = depth;
                }
                let need = chan_bits as usize * n * width;
                if !r.has(need) {
                    return Err(Error("uncompressed frame runs past the packet"));
                }
                for i in 0..n {
                    self.mix_u[i] = extend(r.read(chan_bits) as i32, chan_bits);
                    if pair {
                        self.mix_v[i] = extend(r.read(chan_bits) as i32, chan_bits);
                    }
                }
                mix_bits = 0;
                mix_res = 0;
                bytes_shifted = 0;
            }

            // Always defined, never stale. When there is no shift the buffer is
            // zeroes, so the ORs below are the identity rather than whatever
            // the previous frame left behind.
            let shift_words = n * width;
            self.shift[..shift_words].fill(0);
            if bytes_shifted != 0 {
                let mut s = BitReader::new(packet);
                s.seek(shift_start);
                let w = bytes_shifted * 8;
                for word in &mut self.shift[..shift_words] {
                    *word = s.read(w) as u16;
                }
            }

            let shift = bytes_shifted * 8;
            let mix_u = &self.mix_u[..n];
            let mix_v = &self.mix_v[..n];
            let low = &self.shift[..shift_words];

            if !pair {
                for i in 0..n {
                    out[i * channels + channel_index] = (mix_u[i] << shift) | i32::from(low[i]);
                }
            } else if mix_res != 0 {
                for i in 0..n {
                    let u = mix_u[i];
                    let v = mix_v[i];
                    let l = u
                        .wrapping_add(v)
                        .wrapping_sub(mix_res.wrapping_mul(v) >> mix_bits);
                    let rr = l.wrapping_sub(v);
                    let at = i * channels + channel_index;
                    out[at] = (l << shift) | i32::from(low[i * 2]);
                    out[at + 1] = (rr << shift) | i32::from(low[i * 2 + 1]);
                }
            } else {
                for i in 0..n {
                    let at = i * channels + channel_index;
                    out[at] = (mix_u[i] << shift) | i32::from(low[i * 2]);
                    out[at + 1] = (mix_v[i] << shift) | i32::from(low[i * 2 + 1]);
                }
            }

            channel_index += width;
            produced = samples;
        }

        if produced == 0 {
            return Err(Error("the frame produced no samples"));
        }

        // A frame that stopped early leaves the rest silent rather than
        // undefined.
        for ch in channel_index..channels {
            for i in 0..produced as usize {
                out[i * channels + ch] = 0;
            }
        }

        Ok(produced)
    }
}

// ------------------------------------------------------------------ tests
//
// The decoder's refusals, which are most of what it does. Its bit-exactness is
// established elsewhere and against real files -- docs/formats.md has the
// matrix, and the build checks it by hashing a decode against the WAV that was
// encoded. What is checked here is the other half: that a cookie or a packet
// describing something impossible is turned away rather than acted on. Those
// are the paths a fuzzer explores and a listener never does, so they are the
// ones a regression can hide in.

#[cfg(test)]
mod tests {
    use super::*;

    /// A real cookie, taken from a 44.1 kHz 24-bit stereo file written by
    /// Apple's reference encoder.
    fn good_cookie() -> [u8; 24] {
        [
            0x00, 0x00, 0x10, 0x00, // frameLength 4096
            0x00, // compatibleVersion
            0x18, // bitDepth 24
            0x28, 0x0a, 0x0e, // pb, mb, kb
            0x02, // channels
            0x00, 0xff, // maxRun
            0x00, 0x00, 0x00, 0x00, // maxFrameBytes
            0x00, 0x00, 0x00, 0x00, // avgBitRate
            0x00, 0x00, 0xac, 0x44, // sampleRate 44100
        ]
    }

    #[test]
    fn a_real_cookie_parses_into_the_values_it_encodes() {
        let cfg = Config::parse(&good_cookie()).expect("a real cookie parses");
        assert_eq!(cfg.frame_length, 4096);
        assert_eq!(cfg.bit_depth, 24);
        assert_eq!(cfg.channels, 2);
        assert_eq!(cfg.sample_rate, 44100);
        assert_eq!(cfg.max_run, 255);
    }

    #[test]
    fn too_short_to_be_a_cookie_at_all() {
        let cookie = good_cookie();
        assert!(Config::parse(&cookie[..23]).is_err());
        assert!(Config::parse(&[]).is_err());
    }

    #[test]
    fn a_bit_depth_alac_does_not_define() {
        // 16, 20, 24 and 32 are the format's; everything else here would reach
        // an output path that does not exist.
        for depth in [0u8, 8, 17, 31, 64] {
            let mut cookie = good_cookie();
            cookie[5] = depth;
            assert!(Config::parse(&cookie).is_err(), "depth {depth}");
        }
    }

    #[test]
    fn more_channels_than_alac_has_a_layout_for() {
        let mut cookie = good_cookie();
        cookie[9] = 9;
        assert!(Config::parse(&cookie).is_err());
        cookie[9] = 0;
        assert!(Config::parse(&cookie).is_err());
    }

    #[test]
    fn a_frame_length_that_is_really_an_allocation_request() {
        let mut cookie = good_cookie();
        cookie[..4].copy_from_slice(&[0x7f, 0xff, 0xff, 0xff]);
        assert!(Config::parse(&cookie).is_err());
    }

    #[test]
    fn a_compatible_version_this_decoder_has_never_seen() {
        let mut cookie = good_cookie();
        cookie[4] = 1;
        assert!(Config::parse(&cookie).is_err());
    }

    #[test]
    fn every_layout_is_a_permutation_not_a_rearrangement_that_loses_one() {
        // The bug this guards against is the one Media Foundation ships: a
        // table that moves channels around and drops or duplicates one is
        // indetectable by ear on most material and produces exactly the wrong
        // speaker feed.
        for channels in 1u8..=8 {
            let l = layout_for(channels);
            let mut seen = [0u32; 8];
            for slot in 0..usize::from(channels) {
                let from = usize::from(l.from[slot]);
                assert!(
                    from < usize::from(channels),
                    "channels = {channels}, slot {slot}"
                );
                seen[from] += 1;
            }
            for (c, &count) in seen.iter().enumerate().take(usize::from(channels)) {
                assert_eq!(count, 1, "channels = {channels}, source channel {c}");
            }
            // Mono and stereo report no mask, matching every other decoder here.
            if channels <= 2 {
                assert_eq!(l.mask, 0);
            } else {
                assert_eq!(
                    l.mask.count_ones(),
                    u32::from(channels),
                    "channels = {channels}"
                );
            }
        }
    }

    fn decoder_and_buffer() -> (Decoder, Vec<i32>) {
        let cfg = Config::parse(&good_cookie()).unwrap();
        let decoder = Decoder::new(&cfg).unwrap();
        let out = vec![0i32; cfg.frame_length as usize * usize::from(cfg.channels)];
        (decoder, out)
    }

    #[test]
    fn an_empty_packet_produces_nothing() {
        let (mut decoder, mut out) = decoder_and_buffer();
        assert!(decoder.decode(&[], &mut out).is_err());
    }

    #[test]
    fn bytes_that_are_not_an_alac_frame_produce_nothing_or_a_frame_and_never_more() {
        let (mut decoder, mut out) = decoder_and_buffer();
        let junk: Vec<u8> = (0..512u32).map(|i| (i * 37 + 11) as u8).collect();
        // The only requirement is that it returns rather than misbehaves; a
        // hostile packet is allowed to look decodable.
        if let Ok(frames) = decoder.decode(&junk, &mut out) {
            assert!(frames <= decoder.frame_length());
        }
    }

    #[test]
    fn a_buffer_smaller_than_a_frame_is_refused_before_anything_is_written() {
        let (mut decoder, _) = decoder_and_buffer();
        let mut short = vec![0i32; 10];
        assert!(decoder
            .decode(&[0x20, 0, 0, 0, 0, 0, 0, 0], &mut short)
            .is_err());
    }

    #[test]
    fn an_uncompressed_stereo_frame_decodes_to_the_samples_it_carries() {
        // Built by hand from the bitstream syntax: a CPE, escaped, four samples
        // of 24-bit audio. The values are chosen so that a sign-extension or a
        // channel swap would show.
        let cfg = Config {
            frame_length: 4,
            bit_depth: 24,
            channels: 2,
            sample_rate: 44100,
            pb: 40,
            mb: 10,
            kb: 14,
            ..Config::default()
        };
        let mut decoder = Decoder::new(&cfg).unwrap();

        struct W {
            bytes: Vec<u8>,
            bit: usize,
        }
        impl W {
            fn put(&mut self, n: u32, v: u32) {
                for i in (0..n).rev() {
                    if self.bit.is_multiple_of(8) {
                        self.bytes.push(0);
                    }
                    if (v >> i) & 1 != 0 {
                        let last = self.bytes.len() - 1;
                        self.bytes[last] |= 0x80 >> (self.bit % 8);
                    }
                    self.bit += 1;
                }
            }
        }
        let mut w = W {
            bytes: Vec::new(),
            bit: 0,
        };
        w.put(3, ID_CPE);
        w.put(4, 0); // instance tag
        w.put(12, 0); // reserved
        w.put(4, 0b0001); // not partial, no shift, escaped
        let left: [i32; 4] = [1, -1, 0x7fffff, -0x800000];
        let right: [i32; 4] = [-2, 2, 0x123456, -0x123456];
        for i in 0..4 {
            w.put(24, left[i] as u32 & 0xffffff);
            w.put(24, right[i] as u32 & 0xffffff);
        }
        w.put(3, ID_END);

        let mut out = vec![0i32; 8];
        let frames = decoder
            .decode(&w.bytes, &mut out)
            .expect("a well-formed frame decodes");
        assert_eq!(frames, 4);
        for i in 0..4 {
            assert_eq!(out[i * 2], left[i], "left sample {i}");
            assert_eq!(out[i * 2 + 1], right[i], "right sample {i}");
        }
    }

    #[test]
    fn the_bit_reader_pads_with_zeroes_and_notices_consumption_past_the_end() {
        let data = [0b1010_0000u8, 0xff];
        let mut r = BitReader::new(&data);
        assert_eq!(r.read(3), 0b101);
        assert_eq!(r.read(5), 0);
        assert_eq!(r.read(8), 0xff);
        assert!(!r.exhausted());
        assert_eq!(r.read(4), 0);
        assert!(r.exhausted());
        assert!(!r.has(1));
    }
}

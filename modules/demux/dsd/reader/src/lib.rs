// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSD files, which are two wrappers around the same bitstream.
//
// **DSF and DFF are one format in two headers**, which is why they are one
// module rather than two. Sony's DSF and Philips' DSDIFF both carry raw DSD --
// a one-bit stream at 2.8224 MHz and up -- and differ in the header around it,
// the byte order of the bits, and whether the channels are blocked or
// interleaved. Everything past those three facts is identical, and a reader
// that knew only one of them would be the same reader with a different
// constant.
//
// | | DSF (Sony) | DFF (Philips DSDIFF) |
// |---|---|---|
// | Byte order | little-endian header | big-endian, IFF-shaped, 64-bit sizes |
// | Bit order | **LSB first**, usually: the `bits per sample` field says | **MSB first**, always |
// | Channels | **blocked**: 4096 bytes of one channel, then the next | **interleaved**: one byte each, round-robin |
// | Length | a sample count, in DSD bits | the size of the audio chunk |
// | Compression | none | `DSD ` uncompressed or `DST ` compressed |
//
// This reader hands out **byte-interleaved DSD, MSB first**, whichever it read:
// the blocking is framing and the byte order is a container's statement about
// its own bytes, so both are undone here rather than passed to the codec as a
// flag it would have to be told about. What reaches `codec_dsd` from either
// wrapper is the same bytes, which is the same arrangement `demux_adts` and
// `demux_mp4` have with `codec_aac`.
//
// **DST is refused.** It is a separate arithmetic-coded compression with its own
// several thousand lines, and a file that uses it goes to demux_ffmpeg, which is
// what the fallback chain is for.

#![forbid(unsafe_code)]

use std::io::{self, Read, Seek, SeekFrom};

/// Bytes of one channel in one packet. DSF's own block is this, so a DSF packet
/// is exactly one block group and a DFF packet is made to match.
pub const PACKET_BYTES_PER_CHANNEL: usize = 4096;

/// The most channels either wrapper names.
pub const MAX_CHANNELS: usize = 6;

/// A DSD rate is a multiple of 44100 * 64. Anything else is not a rate a DSD
/// file has, and a header claiming one is a header to decline.
const DSD64: u32 = 2_822_400;

/// The largest DSD rate this accepts: DSD1024. Past that the number is being
/// used to make an allocation rather than to describe audio.
const MAX_RATE: u32 = DSD64 * 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub &'static str);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for Error {}

/// Which wrapper a file turned out to be. Reported so a log can say it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Wrapper {
    Dsf,
    Dff,
}

impl Wrapper {
    pub fn name(self) -> &'static str {
        match self {
            Wrapper::Dsf => "DSF",
            Wrapper::Dff => "DSDIFF",
        }
    }
}

/// What the header said.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Info {
    pub wrapper: Wrapper,
    /// DSD samples per second: 2822400 for DSD64.
    pub dsd_rate: u32,
    pub channels: u32,
    /// MP_SPEAKER_* bits, or 0 when there is no layout to name.
    pub channel_mask: u32,
    /// Bytes of DSD per channel in the whole file, which is the graph's frame
    /// count because a frame here is one byte.
    pub total_bytes_per_channel: u64,
}

impl Info {
    /// The rate the graph counts in: bytes, not DSD samples.
    pub fn byte_rate(&self) -> u32 {
        self.dsd_rate / 8
    }
}

/// Whether `head` begins a DSD file: 100 for either wrapper, 0 otherwise.
///
/// Four bytes are enough and no more are needed. `DSD ` and `FRM8` are both
/// unambiguous at offset zero, and neither turns up there by chance -- which is
/// the opposite of the frame-header formats, where a probe has to find two
/// frames that agree before it may believe one.
pub fn probe(head: &[u8]) -> u32 {
    if head.len() < 12 {
        return 0;
    }
    if &head[..4] == b"DSD " {
        return 100;
    }
    // FRM8's form type is at offset 12, past the 8-byte size.
    if &head[..4] == b"FRM8" && &head[12..16.min(head.len())] == b"DSD " {
        return 100;
    }
    0
}

/// The speaker mask for a channel count, in the order both wrappers use.
///
/// DSF's channel types and DSDIFF's channel ids agree with each other and with
/// WAVE for every layout they name, which is the one piece of luck in this
/// format: 5.1 is front left, front right, centre, LFE, back left, back right
/// in all three. So there is no permutation here, unlike ALAC and AAC, and this
/// is a mask rather than a mapping.
fn mask_for(channels: u32) -> u32 {
    const FL: u32 = 0x1;
    const FR: u32 = 0x2;
    const FC: u32 = 0x4;
    const LFE: u32 = 0x8;
    const BL: u32 = 0x10;
    const BR: u32 = 0x20;
    match channels {
        // Mono and stereo take the non-extensible form, as everywhere else here.
        1 | 2 => 0,
        3 => FL | FR | FC,
        4 => FL | FR | BL | BR,
        5 => FL | FR | FC | LFE,
        6 => FL | FR | FC | LFE | BL | BR,
        _ => 0,
    }
}

/// Reverses the bits of every byte, which is what turns DSF's LSB-first storage
/// into the MSB-first order everything downstream uses.
///
/// A table rather than `u8::reverse_bits` in a loop, because this runs over
/// every byte of the file: at DSD256 that is 1.4 MB a second per channel.
static REVERSED: [u8; 256] = {
    let mut t = [0u8; 256];
    let mut i = 0usize;
    while i < 256 {
        t[i] = (i as u8).reverse_bits();
        i += 1;
    }
    t
};

// ------------------------------------------------------------ little-endian

fn u32_le(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

fn u64_le(b: &[u8]) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[..8]);
    u64::from_le_bytes(v)
}

fn u64_be(b: &[u8]) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[..8]);
    u64::from_be_bytes(v)
}

// ------------------------------------------------------------- the reader

/// A DSD file over any seekable reader.
pub struct Reader<R> {
    inner: R,
    info: Info,
    /// Where the audio starts.
    audio_at: u64,
    /// Bytes of audio, per channel, that are really there -- which is not the
    /// header's claim when the file is short.
    have_bytes_per_channel: u64,
    /// DSF only: bytes of one channel in one block.
    block: usize,
    /// Whether the stored bits need reversing.
    lsb_first: bool,
    /// The next packet's first byte, per channel.
    at_bytes: u64,
    /// One block group, read whole before de-interleaving.
    scratch: Vec<u8>,
}

impl<R: Read + Seek> Reader<R> {
    /// Reads the header. `Ok(None)` when this is not a DSD file, an error only
    /// when the reader itself failed.
    pub fn open(mut inner: R) -> io::Result<Result<Reader<R>, Error>> {
        let mut head = [0u8; 16];
        inner.seek(SeekFrom::Start(0))?;
        if !read_exact_or_eof(&mut inner, &mut head)? {
            return Ok(Err(Error("too short to be a DSD file")));
        }
        let end = inner.seek(SeekFrom::End(0))?;
        if &head[..4] == b"DSD " {
            Self::open_dsf(inner, end)
        } else if &head[..4] == b"FRM8" && &head[12..16] == b"DSD " {
            Self::open_dff(inner, end)
        } else {
            Ok(Err(Error("not DSF and not DSDIFF")))
        }
    }

    fn finish(
        inner: R,
        info: Info,
        audio_at: u64,
        claimed_bytes_per_channel: u64,
        file_end: u64,
        block: usize,
        lsb_first: bool,
    ) -> io::Result<Result<Reader<R>, Error>> {
        if info.channels == 0 || info.channels as usize > MAX_CHANNELS {
            return Ok(Err(Error("a channel count no DSD wrapper names")));
        }
        if info.dsd_rate < DSD64 || info.dsd_rate > MAX_RATE || !info.dsd_rate.is_multiple_of(DSD64)
        {
            return Ok(Err(Error("a rate that is not a multiple of DSD64")));
        }
        if audio_at > file_end {
            return Ok(Err(Error("the audio starts past the end of the file")));
        }
        // **What is there, not what was claimed.** A truncated file states the
        // length it was going to have, and believing it turns the end of the
        // track into a read past the data. The two are reconciled here, once,
        // so nothing below has to keep asking.
        let on_disk = (file_end - audio_at) / u64::from(info.channels);
        // Rounded down to a whole DoP frame, for the reason `read_packet`
        // gives: an odd trailing byte is not a frame and is never handed out,
        // so a length that counted it would promise audio that never arrives.
        let have = claimed_bytes_per_channel.min(on_disk) & !1u64;
        let mut info = info;
        info.total_bytes_per_channel = have;
        Ok(Ok(Reader {
            inner,
            info,
            audio_at,
            have_bytes_per_channel: have,
            block,
            lsb_first,
            at_bytes: 0,
            scratch: Vec::new(),
        }))
    }

    /// DSF: a fixed 92-byte header, then blocks.
    fn open_dsf(mut inner: R, file_end: u64) -> io::Result<Result<Reader<R>, Error>> {
        let mut h = [0u8; 92];
        inner.seek(SeekFrom::Start(0))?;
        if !read_exact_or_eof(&mut inner, &mut h)? {
            return Ok(Err(Error("a DSF header that stops early")));
        }
        if &h[28..32] != b"fmt " || &h[80..84] != b"data" {
            return Ok(Err(Error("a DSF file with no fmt or no data chunk")));
        }
        let version = u32_le(&h[40..]);
        let format_id = u32_le(&h[44..]);
        if version != 1 || format_id != 0 {
            return Ok(Err(Error(
                "a DSF format version or id this reader does not know",
            )));
        }
        let channels = u32_le(&h[52..]);
        let dsd_rate = u32_le(&h[56..]);
        let bits_per_sample = u32_le(&h[60..]);
        let sample_count = u64_le(&h[64..]); // DSD samples per channel, so bits
        let block = u32_le(&h[72..]) as usize;

        // **1 means LSB first and 8 means MSB first**, which is a field that
        // reads backwards until you know that it is counting the bit position
        // of the earliest sample rather than a width. Almost every .dsf says 1.
        let lsb_first = match bits_per_sample {
            1 => true,
            8 => false,
            _ => return Ok(Err(Error("a DSF bits-per-sample that is neither 1 nor 8"))),
        };
        if block == 0 || block > 1 << 20 || !block.is_multiple_of(8) {
            return Ok(Err(Error("a DSF block size that is not a block size")));
        }
        let info = Info {
            wrapper: Wrapper::Dsf,
            dsd_rate,
            channels,
            channel_mask: mask_for(channels),
            total_bytes_per_channel: 0,
        };
        Self::finish(
            inner,
            info,
            92,
            sample_count / 8,
            file_end,
            block,
            lsb_first,
        )
    }

    /// DSDIFF: IFF chunks with 64-bit big-endian sizes.
    fn open_dff(mut inner: R, file_end: u64) -> io::Result<Result<Reader<R>, Error>> {
        let mut rate = 0u32;
        let mut channels = 0u32;
        let mut uncompressed = false;
        let mut audio_at = 0u64;
        let mut audio_bytes = 0u64;

        // Chunks of the FRM8 form. Its header is four bytes of id and eight of
        // size, and then a **form type** -- `DSD ` -- which is part of the
        // form's body rather than of its header. So the first chunk is at 16,
        // and starting at 12 reads that form type as a chunk id and the next
        // chunk's id as its size, which is how this was found.
        let mut at = 16u64;
        // Bounded: a file made of nothing but empty chunk headers should end
        // this walk rather than occupy it.
        for _ in 0..4096 {
            if at.saturating_add(12) > file_end {
                break;
            }
            inner.seek(SeekFrom::Start(at))?;
            let mut head = [0u8; 12];
            if !read_exact_or_eof(&mut inner, &mut head)? {
                break;
            }
            let id = [head[0], head[1], head[2], head[3]];
            let size = u64_be(&head[4..]);
            let body = at + 12; // bounded above
            if size > file_end.saturating_sub(body) && &id != b"DSD " {
                // A chunk claiming more than the file holds. The audio chunk is
                // allowed to, because a file still being written says so; every
                // other chunk is a header this reader will not act on.
                break;
            }
            match &id {
                b"PROP" => {
                    // PROP's body is a four-byte type and then more chunks.
                    let mut kind = [0u8; 4];
                    if read_exact_or_eof(&mut inner, &mut kind)? && &kind == b"SND " {
                        read_prop(
                            &mut inner,
                            body.saturating_add(4),
                            body.saturating_add(size),
                            &mut rate,
                            &mut channels,
                        )?;
                    }
                }
                b"CMPR" => {}
                b"DSD " => {
                    audio_at = body;
                    audio_bytes = size;
                    uncompressed = true;
                }
                b"DST " => {
                    return Ok(Err(Error(
                        "DSDIFF with DST compression, which demux_ffmpeg reads and this does not",
                    )));
                }
                _ => {}
            }
            // Chunks are padded to an even length, and the pad byte is not
            // counted in the size. Forgetting that walks into the middle of the
            // next chunk, which is a header made of audio.
            // **Saturating, because `size` is a number the file claimed.**
            // The audio chunk is exempted from the bounds check above -- a file
            // still being written states a length it has not reached yet -- so
            // a DSDIFF whose `DSD ` chunk claims 2^64 bytes reaches here with
            // it. Found by `dsd_fuzzer`, which builds with overflow checks on
            // for exactly this: in a release build the wrap would have been
            // silent and the walk would have restarted inside the audio.
            at = body.saturating_add(size).saturating_add(size & 1);
        }

        if !uncompressed {
            return Ok(Err(Error("a DSDIFF file with no uncompressed DSD chunk")));
        }
        if channels == 0 || rate == 0 {
            return Ok(Err(Error(
                "a DSDIFF file whose PROP names no rate or no channels",
            )));
        }
        let info = Info {
            wrapper: Wrapper::Dff,
            dsd_rate: rate,
            channels,
            channel_mask: mask_for(channels),
            total_bytes_per_channel: 0,
        };
        let per_channel = audio_bytes / u64::from(channels.max(1));
        Self::finish(inner, info, audio_at, per_channel, file_end, 0, false)
    }

    pub fn info(&self) -> &Info {
        &self.info
    }

    /// Bytes one packet needs: `PACKET_BYTES_PER_CHANNEL` per channel, or fewer
    /// at the end of the file.
    pub fn max_packet_bytes(&self) -> usize {
        PACKET_BYTES_PER_CHANNEL * self.info.channels as usize
    }

    /// The next packet, byte-interleaved and MSB first, into `dst`.
    ///
    /// Returns the bytes written and the packet's first DSD byte, or `None` at
    /// the end of the audio. `dst` must be at least `max_packet_bytes()`.
    ///
    /// **Always an even number of bytes per channel.** Two DSD bytes make one
    /// DoP frame, and a packet with an odd byte in it would leave half a frame
    /// for the next one to finish -- so the odd byte is not handed out at all.
    /// At most one byte per channel is lost, at the end of a file, which is
    /// 0.35 microseconds at DSD64 and is already excluded from the length this
    /// reader reports.
    pub fn read_packet(&mut self, dst: &mut [u8]) -> io::Result<Option<(usize, u64)>> {
        let channels = self.info.channels as usize;
        let left = self.have_bytes_per_channel.saturating_sub(self.at_bytes);
        if left == 0 {
            return Ok(None);
        }
        let want = (left as usize).min(PACKET_BYTES_PER_CHANNEL) & !1usize;
        if want == 0 {
            return Ok(None); // a single byte left, which is not a DoP frame
        }
        let out_bytes = want * channels;
        if dst.len() < out_bytes {
            return Ok(Some((0, 0))); // the caller asks again with room
        }
        let first_frame = self.at_bytes;

        match self.info.wrapper {
            // Interleaved already: one read, and the bits are already MSB first.
            Wrapper::Dff => {
                let at = self
                    .audio_at
                    .saturating_add(self.at_bytes.saturating_mul(channels as u64));
                self.inner.seek(SeekFrom::Start(at))?;
                let got = read_up_to(&mut self.inner, &mut dst[..out_bytes])?;
                if got == 0 {
                    return Ok(None);
                }
                let per_channel = (got / channels) & !1usize;
                if per_channel == 0 {
                    return Ok(None);
                }
                self.at_bytes += per_channel as u64;
                return Ok(Some((per_channel * channels, first_frame)));
            }
            // Blocked: read whole blocks and interleave them here.
            Wrapper::Dsf => {}
        }

        // DSF. The file is laid out as block groups -- `block` bytes of channel
        // 0, then of channel 1, and so on -- so a packet is read a group at a
        // time and woven. `want` is at most one block, because the packet size
        // and the block size are the same number.
        let group = self.block * channels;
        let group_index = self.at_bytes / self.block as u64;
        let within = (self.at_bytes % self.block as u64) as usize;
        self.scratch.resize(group, 0);
        self.inner
            .seek(SeekFrom::Start(
                self.audio_at
                    .saturating_add(group_index.saturating_mul(group as u64)),
            ))?;
        let got = read_up_to(&mut self.inner, &mut self.scratch)?;
        if got <= within {
            return Ok(None);
        }

        // How much of this group is really there, per channel.
        let per_channel_here = (got / channels).min(self.block);
        let take = want.min(per_channel_here.saturating_sub(within)) & !1usize;
        if take == 0 {
            return Ok(None);
        }
        for i in 0..take {
            for c in 0..channels {
                let from = c * self.block + within + i;
                let byte = self.scratch.get(from).copied().unwrap_or(0);
                dst[i * channels + c] = if self.lsb_first {
                    REVERSED[usize::from(byte)]
                } else {
                    byte
                };
            }
        }
        self.at_bytes += take as u64;
        Ok(Some((take * channels, first_frame)))
    }

    /// To the packet holding byte `frame` of each channel.
    ///
    /// **No pre-roll and no sync points.** A DSD byte depends on nothing before
    /// it -- there is no predictor, no window and no reservoir -- so every byte
    /// is a seek target and the host needs nothing warmed up. It is the one
    /// format here where that is true of the bitstream itself rather than of a
    /// decoder that has been written carefully.
    pub fn seek(&mut self, frame: u64) -> io::Result<()> {
        self.at_bytes = frame.min(self.have_bytes_per_channel);
        Ok(())
    }
}

/// PROP's sub-chunks: the rate and the channel count, and nothing else this
/// reader acts on.
fn read_prop<R: Read + Seek>(
    inner: &mut R,
    mut at: u64,
    end: u64,
    rate: &mut u32,
    channels: &mut u32,
) -> io::Result<()> {
    for _ in 0..256 {
        if at.saturating_add(12) > end {
            break;
        }
        inner.seek(SeekFrom::Start(at))?;
        let mut head = [0u8; 12];
        if !read_exact_or_eof(inner, &mut head)? {
            break;
        }
        let id = [head[0], head[1], head[2], head[3]];
        let size = u64_be(&head[4..]);
        let body = at + 12; // bounded above
        if size > end.saturating_sub(body) {
            break;
        }
        match &id {
            b"FS  " if size >= 4 => {
                let mut v = [0u8; 4];
                if read_exact_or_eof(inner, &mut v)? {
                    *rate = u32::from_be_bytes(v);
                }
            }
            b"CHNL" if size >= 2 => {
                let mut v = [0u8; 2];
                if read_exact_or_eof(inner, &mut v)? {
                    *channels = u32::from(u16::from_be_bytes(v));
                }
            }
            _ => {}
        }
        at = body.saturating_add(size).saturating_add(size & 1);
    }
    Ok(())
}

/// Fills `buf` completely, or says it could not. A short read at the end of a
/// file is not an error here -- it is the end of the file -- so it is a `bool`
/// rather than an `Err`.
fn read_exact_or_eof<R: Read>(inner: &mut R, buf: &mut [u8]) -> io::Result<bool> {
    Ok(read_up_to(inner, buf)? == buf.len())
}

fn read_up_to<R: Read>(inner: &mut R, buf: &mut [u8]) -> io::Result<usize> {
    let mut got = 0usize;
    while got < buf.len() {
        match inner.read(&mut buf[got..]) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(got)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// The reader for these bytes, or None when it declined them. The tests
    /// say `opened(...)` rather than `unwrap()` because `Reader` holds the
    /// stream and has no `Debug` to print -- and nothing it could print would
    /// say more than the error already does.
    fn opened(bytes: Vec<u8>) -> Option<Reader<Cursor<Vec<u8>>>> {
        Reader::open(Cursor::new(bytes)).unwrap().ok()
    }

    /// Why the reader declined these bytes.
    fn declined(bytes: Vec<u8>) -> Error {
        Reader::open(Cursor::new(bytes))
            .unwrap()
            .err()
            .expect("these bytes should not have opened")
    }

    /// A DSF file: `blocks` block groups of `block` bytes per channel, every
    /// byte of channel `c` in group `g` set to a value that names both.
    fn dsf(channels: u32, block: usize, blocks: usize, lsb_first: bool) -> Vec<u8> {
        let audio = block * channels as usize * blocks;
        let mut f = Vec::new();
        f.extend_from_slice(b"DSD ");
        f.extend_from_slice(&28u64.to_le_bytes());
        f.extend_from_slice(&((92 + audio) as u64).to_le_bytes());
        f.extend_from_slice(&0u64.to_le_bytes());
        f.extend_from_slice(b"fmt ");
        f.extend_from_slice(&52u64.to_le_bytes());
        f.extend_from_slice(&1u32.to_le_bytes()); // version
        f.extend_from_slice(&0u32.to_le_bytes()); // format id: DSD raw
        f.extend_from_slice(&channels.to_le_bytes()); // channel type
        f.extend_from_slice(&channels.to_le_bytes());
        f.extend_from_slice(&DSD64.to_le_bytes());
        f.extend_from_slice(&(if lsb_first { 1u32 } else { 8u32 }).to_le_bytes());
        f.extend_from_slice(&((block * blocks * 8) as u64).to_le_bytes()); // in bits
        f.extend_from_slice(&(block as u32).to_le_bytes());
        f.extend_from_slice(&0u32.to_le_bytes());
        f.extend_from_slice(b"data");
        f.extend_from_slice(&((12 + audio) as u64).to_le_bytes());
        for g in 0..blocks {
            for c in 0..channels as usize {
                for i in 0..block {
                    f.push(((g * 16 + c) as u8).wrapping_add((i & 1) as u8));
                }
            }
        }
        f
    }

    /// A DSDIFF file with the same audio, interleaved rather than blocked.
    fn dff(channels: u32, bytes_per_channel: usize) -> Vec<u8> {
        let mut props = Vec::new();
        props.extend_from_slice(b"SND ");
        props.extend_from_slice(b"FS  ");
        props.extend_from_slice(&4u64.to_be_bytes());
        props.extend_from_slice(&DSD64.to_be_bytes());
        props.extend_from_slice(b"CHNL");
        props.extend_from_slice(&(2 + 4 * u64::from(channels)).to_be_bytes());
        props.extend_from_slice(&(channels as u16).to_be_bytes());
        for _ in 0..channels {
            props.extend_from_slice(b"SLFT");
        }

        let mut audio = Vec::new();
        for i in 0..bytes_per_channel {
            for c in 0..channels as usize {
                audio.push(((c * 3) as u8).wrapping_add((i & 7) as u8));
            }
        }

        let mut body = Vec::new();
        body.extend_from_slice(b"DSD "); // the form type
        body.extend_from_slice(b"FVER");
        body.extend_from_slice(&4u64.to_be_bytes());
        body.extend_from_slice(&0x0104_0000u32.to_be_bytes());
        body.extend_from_slice(b"PROP");
        body.extend_from_slice(&(props.len() as u64).to_be_bytes());
        body.extend_from_slice(&props);
        body.extend_from_slice(b"DSD ");
        body.extend_from_slice(&(audio.len() as u64).to_be_bytes());
        body.extend_from_slice(&audio);

        let mut f = Vec::new();
        f.extend_from_slice(b"FRM8");
        f.extend_from_slice(&(body.len() as u64).to_be_bytes());
        f.extend_from_slice(&body);
        f
    }

    #[test]
    fn a_dsf_header_parses_into_what_it_states() {
        let r = opened(dsf(2, 4096, 3, true)).unwrap();
        let info = *r.info();
        assert_eq!(info.wrapper, Wrapper::Dsf);
        assert_eq!(info.dsd_rate, 2_822_400);
        assert_eq!(info.byte_rate(), 352_800);
        assert_eq!(info.channels, 2);
        assert_eq!(info.channel_mask, 0); // stereo takes the non-extensible form
        assert_eq!(info.total_bytes_per_channel, 4096 * 3);
    }

    #[test]
    fn a_dsdiff_header_parses_into_what_it_states() {
        let r = opened(dff(2, 9000)).unwrap();
        let info = *r.info();
        assert_eq!(info.wrapper, Wrapper::Dff);
        assert_eq!(info.dsd_rate, 2_822_400);
        assert_eq!(info.channels, 2);
        assert_eq!(info.total_bytes_per_channel, 9000);
    }

    #[test]
    fn five_point_one_gets_the_wave_mask_because_the_orders_agree() {
        let r = opened(dsf(6, 4096, 1, true)).unwrap();
        assert_eq!(r.info().channel_mask, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20);
    }

    #[test]
    fn dsf_blocks_come_out_interleaved_and_msb_first() {
        // Written LSB first, so every byte must come back reversed.
        let mut r = opened(dsf(2, 4096, 2, true)).unwrap();
        let mut buf = vec![0u8; r.max_packet_bytes()];
        let (bytes, frame) = r.read_packet(&mut buf).unwrap().unwrap();
        assert_eq!(bytes, 4096 * 2);
        assert_eq!(frame, 0);
        // Group 0: channel 0 is 0,1,0,1..., channel 1 is 1,2,1,2..., each
        // reversed, and the two woven together.
        for i in 0..4 {
            assert_eq!(buf[i * 2], REVERSED[usize::from((i & 1) as u8)]);
            assert_eq!(buf[i * 2 + 1], REVERSED[usize::from(1 + (i & 1) as u8)]);
        }
        let (_, frame) = r.read_packet(&mut buf).unwrap().unwrap();
        assert_eq!(frame, 4096);
        assert!(r.read_packet(&mut buf).unwrap().is_none());
    }

    #[test]
    fn msb_first_dsf_is_not_reversed() {
        let mut r = opened(dsf(1, 8, 1, false)).unwrap();
        let mut buf = vec![0u8; r.max_packet_bytes()];
        let (bytes, _) = r.read_packet(&mut buf).unwrap().unwrap();
        assert_eq!(bytes, 8);
        assert_eq!(&buf[..4], &[0, 1, 0, 1]);
    }

    #[test]
    fn dsdiff_bytes_come_out_as_they_went_in() {
        let mut r = opened(dff(2, 100)).unwrap();
        let mut buf = vec![0u8; r.max_packet_bytes()];
        let (bytes, frame) = r.read_packet(&mut buf).unwrap().unwrap();
        assert_eq!(bytes, 200);
        assert_eq!(frame, 0);
        assert_eq!(&buf[..4], &[0, 3, 1, 4]);
        assert!(r.read_packet(&mut buf).unwrap().is_none());
    }

    #[test]
    fn a_seek_lands_on_the_byte_it_was_given_because_dsd_has_no_history() {
        let mut r = opened(dsf(2, 4096, 4, true)).unwrap();
        let mut buf = vec![0u8; r.max_packet_bytes()];
        r.seek(2 * 4096).unwrap();
        let (_, frame) = r.read_packet(&mut buf).unwrap().unwrap();
        assert_eq!(frame, 2 * 4096);
        // Group 2, channel 0, which the generator wrote as 32, 33, 32, 33...
        assert_eq!(buf[0], REVERSED[32]);
        // Past the end is the end, not an error.
        r.seek(1 << 40).unwrap();
        assert!(r.read_packet(&mut buf).unwrap().is_none());
    }

    #[test]
    fn a_truncated_file_stops_where_the_bytes_stop_not_where_the_header_says() {
        let mut file = dsf(2, 4096, 4, true);
        file.truncate(92 + 4096 * 2 * 2 + 512); // two whole groups and a bit
        let mut r = opened(file).unwrap();
        // The header claimed four groups; there are two and a fraction.
        assert!(r.info().total_bytes_per_channel < 4 * 4096);
        let mut buf = vec![0u8; r.max_packet_bytes()];
        let mut packets = 0;
        while r.read_packet(&mut buf).unwrap().is_some() {
            packets += 1;
            assert!(packets < 10);
        }
        assert_eq!(packets, 3);
    }

    #[test]
    fn what_is_not_a_dsd_file_is_declined_rather_than_guessed() {
        assert_eq!(probe(&[0u8; 64]), 0);
        assert_eq!(probe(b"RIFF____WAVEfmt "), 0);
        assert_eq!(probe(&dsf(2, 4096, 1, true)[..64]), 100);
        assert_eq!(probe(&dff(2, 64)[..64]), 100);
        assert!(opened(vec![0u8; 4096]).is_none());
    }

    #[test]
    fn a_chunk_that_claims_the_whole_address_space() {
        // What `dsd_fuzzer` found in five minutes. The audio chunk is exempt
        // from the bounds check the other chunks get -- a file still being
        // written states a length it has not reached -- so a claim of 2^64
        // reaches the walk's arithmetic. With overflow checks on that was a
        // panic; without them it would have wrapped and restarted the walk
        // inside the audio, which is the quieter of the two failures and the
        // worse one.
        let mut body = Vec::new();
        body.extend_from_slice(b"DSD ");
        body.extend_from_slice(b"DSD ");
        body.extend_from_slice(&u64::MAX.to_be_bytes());
        body.extend_from_slice(&[0u8; 32]);
        let mut f = Vec::new();
        f.extend_from_slice(b"FRM8");
        f.extend_from_slice(&(body.len() as u64).to_be_bytes());
        f.extend_from_slice(&body);
        // Whatever it decides, it must decide it rather than panic.
        let _ = Reader::open(Cursor::new(f)).unwrap();

        // And the same claim on a chunk that is not the audio.
        let mut body = Vec::new();
        body.extend_from_slice(b"DSD ");
        body.extend_from_slice(b"PROP");
        body.extend_from_slice(&u64::MAX.to_be_bytes());
        body.extend_from_slice(b"SND ");
        let mut f = Vec::new();
        f.extend_from_slice(b"FRM8");
        f.extend_from_slice(&u64::MAX.to_be_bytes());
        f.extend_from_slice(&body);
        let _ = Reader::open(Cursor::new(f)).unwrap();
    }

    #[test]
    fn dst_compression_is_refused_by_name() {
        let mut f = Vec::new();
        let mut body = Vec::new();
        body.extend_from_slice(b"DSD ");
        body.extend_from_slice(b"DST ");
        body.extend_from_slice(&16u64.to_be_bytes());
        body.extend_from_slice(&[0u8; 16]);
        f.extend_from_slice(b"FRM8");
        f.extend_from_slice(&(body.len() as u64).to_be_bytes());
        f.extend_from_slice(&body);
        assert!(declined(f).0.contains("DST"));
    }

    #[test]
    fn a_header_that_claims_a_rate_no_dsd_file_has() {
        let mut file = dsf(2, 4096, 1, true);
        file[56..60].copy_from_slice(&44100u32.to_le_bytes());
        assert!(opened(file).is_none());
        let mut file = dsf(2, 4096, 1, true);
        file[56..60].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(opened(file).is_none());
    }

    #[test]
    fn a_channel_count_no_wrapper_names() {
        let mut file = dsf(2, 4096, 1, true);
        file[52..56].copy_from_slice(&99u32.to_le_bytes());
        assert!(opened(file).is_none());
        let mut file = dsf(2, 4096, 1, true);
        file[52..56].copy_from_slice(&0u32.to_le_bytes());
        assert!(opened(file).is_none());
    }

    #[test]
    fn the_reversal_table_is_a_reversal() {
        for i in 0..256usize {
            assert_eq!(REVERSED[i], (i as u8).reverse_bits());
            assert_eq!(REVERSED[usize::from(REVERSED[i])], i as u8);
        }
    }
}

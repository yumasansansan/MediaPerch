// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raw AAC in ADTS framing, as a container -- the framing alone, over any
// seekable stream.
//
// ADTS is the framing for AAC that has nowhere to keep configuration, so it
// repeats it in front of every frame: the object type, the sample rate index and
// the channel configuration, seven bytes at a time. Those are the same three
// fields an MP4 keeps once in its `AudioSpecificConfig`, so this framer
// assembles that config out of the first header and the module hands the codec
// the same two bytes it would have got from an MP4 -- which is the point of the
// split: `codec_aac` cannot tell which container it was called from, and
// should not be able to.
//
// **No library does this.** It is seven bytes of header and nothing ships a
// reader for those alone, which is why this is written here and why it is
// written in the language where an index past a slice is a panic caught at the
// module boundary rather than a read of whatever came next. It is a port of the
// C++ framer it replaces, accepted on the criterion that every packet came out
// at the same offset with the same bytes -- the format matrix and the seek
// hashes docs/formats.md records did not move.
//
// What it does differently from the C++: where that stepped through a file one
// `fseek` and one nine-byte `fread` per candidate offset, this reads a window
// once and scans it. The answer is the same first confirmed offset; the cost
// is a few hundred kilobytes of memory once at open rather than half a million
// system calls on a file that turns out not to be ADTS.

#![forbid(unsafe_code)]

use std::io::{self, Read, Seek, SeekFrom};

/// One AAC frame is always this many samples for the profiles here.
pub const FRAME_SAMPLES: u64 = 1024;

/// One remembered position in this many frames, as elsewhere.
const INDEX_STRIDE: u64 = 32;

/// How far past a bad byte a resynchronisation looks, in bytes.
const RESYNC_LIMIT: usize = 65536;

/// How far into a file `open` looks for a confirmed frame, in bytes.
const OPEN_SEARCH_LIMIT: usize = 262_144;

/// The largest frame the thirteen-bit length field can name.
const MAX_FRAME_BYTES: usize = 8191;

/// The nine bytes a header can occupy: seven, plus two of CRC.
const HEADER_WINDOW: usize = 9;

const RATES: [u32; 13] = [
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
];

/// The eleven rates AAC's four-bit index can name, plus 96 and 88.2 above them.
pub fn rate_for_index(index: u32) -> u32 {
    RATES.get(index as usize).copied().unwrap_or(0)
}

/// An ADTS header. The same configuration an MP4 keeps in `esds`, repeated in
/// front of every frame because a raw stream has nowhere else to put it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Header {
    pub object_type: u32,
    pub rate_index: u32,
    pub channel_config: u32,
    pub header_bytes: u32,
    pub frame_bytes: u32,
}

impl Header {
    /// The header at the start of `h`, if there is one. Seven bytes are
    /// needed; the CRC, when present, is skipped rather than checked.
    pub fn parse(h: &[u8]) -> Option<Header> {
        if h.len() < 7 {
            return None;
        }
        // Syncword, and the layer field: 0xFF followed by set bits is not
        // rare, and layer must be 00 for ADTS.
        if h[0] != 0xFF || (h[1] & 0xF0) != 0xF0 || (h[1] & 0x06) != 0 {
            return None;
        }
        let protection_absent = (h[1] & 1) != 0;
        let object_type = u32::from((h[2] >> 6) & 0x3) + 1;
        let rate_index = u32::from((h[2] >> 2) & 0xF);
        let channel_config = (u32::from(h[2] & 1) << 2) | u32::from((h[3] >> 6) & 0x3);
        let frame_bytes =
            (u32::from(h[3] & 0x3) << 11) | (u32::from(h[4]) << 3) | (u32::from(h[5]) >> 5);
        let blocks = u32::from(h[6] & 0x3) + 1;
        let header_bytes = if protection_absent { 7 } else { 9 };
        if rate_index > 12 || frame_bytes <= header_bytes || blocks != 1 {
            return None;
        }
        Some(Header {
            object_type,
            rate_index,
            channel_config,
            header_bytes,
            frame_bytes,
        })
    }

    /// Whether two headers describe the same stream. A chance sync agrees
    /// about the sync bits and about very little else.
    pub fn same_stream(&self, other: &Header) -> bool {
        self.object_type == other.object_type
            && self.rate_index == other.rate_index
            && self.channel_config == other.channel_config
    }

    pub fn sample_rate(&self) -> u32 {
        rate_for_index(self.rate_index)
    }

    /// **The AudioSpecificConfig, assembled out of the frame header.** Five
    /// bits of object type, four of rate index, four of channel configuration
    /// and three zeroes for the rest of the GASpecificConfig -- which is byte
    /// for byte what an MP4 would have handed over, so `codec_aac` is handed
    /// the same configuration from either container and cannot tell them
    /// apart.
    pub fn audio_specific_config(&self) -> [u8; 2] {
        [
            ((self.object_type << 3) | (self.rate_index >> 1)) as u8,
            (((self.rate_index & 1) << 7) | (self.channel_config << 3)) as u8,
        ]
    }
}

/// The length of an ID3v2 tag at the start of `head`, header included, or
/// None when `head` does not begin with one.
fn id3v2_length(head: &[u8]) -> Option<usize> {
    if head.len() < 10 || &head[..3] != b"ID3" {
        return None;
    }
    let size = (usize::from(head[6] & 0x7F) << 21)
        | (usize::from(head[7] & 0x7F) << 14)
        | (usize::from(head[8] & 0x7F) << 7)
        | usize::from(head[9] & 0x7F);
    Some(10 + size)
}

/// Whether `head`, the first bytes of a file, is an ADTS stream: 100 when two
/// headers a frame apart agree, 0 otherwise.
pub fn probe(head: &[u8]) -> u32 {
    if head.len() < 16 {
        return 0;
    }
    let mut at = 0usize;
    if let Some(tag) = id3v2_length(head) {
        at = tag;
        if at + 16 > head.len() {
            // **Nothing, not a weak claim.** `demux_mpa` claims 60 here
            // because an MP3 with cover art is most MP3s and its frame header
            // is genuinely out of reach; an ADTS stream with a large ID3v2 tag
            // in front of it is rare enough that the same guess costs more than
            // it buys. Measured: with a speculative 60 this module outranked
            // `demux_mpa` on priority for every big-tagged MP3, opened it,
            // failed, and made the host fall through -- the right answer by the
            // wrong route.
            return 0;
        }
    }

    // Two headers a frame apart that agree, for the reason `demux_mpa` needs
    // the same rule: one sync pattern turns up by chance in any few kilobytes
    // of somebody else's compressed audio.
    let limit = head.len().min(at + 8192);
    for i in at..limit.saturating_sub(6) {
        let Some(here) = Header::parse(&head[i..limit]) else {
            continue;
        };
        let after = i + here.frame_bytes as usize;
        if after + 7 <= head.len() {
            if let Some(next) = Header::parse(&head[after..]) {
                if here.same_stream(&next) {
                    return 100;
                }
            }
        }
    }
    0
}

/// What `Stream::read_packet` hands back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Packet {
    /// `bytes` of raw_data_block were written; `frame` is its first sample.
    Frame { bytes: usize, frame: u64 },
    /// The end of the audio.
    End,
    /// The buffer holds less than this packet needs; nothing was consumed.
    TooSmall(usize),
}

/// An ADTS stream over any seekable reader.
pub struct Stream<R> {
    reader: R,
    /// Where `reader` is, tracked here so a walk never asks the OS.
    pos: u64,
    first: Header,
    audio_at: u64,
    next_index: u64,
    /// Byte offset of frame `i * INDEX_STRIDE`.
    index: Vec<u64>,
}

impl<R: Read + Seek> Stream<R> {
    /// Opens the stream, or says it is not one: `Ok(None)` when no frame
    /// confirmed by the one after it is found in the first quarter megabyte.
    pub fn open(reader: R) -> io::Result<Option<Stream<R>>> {
        let mut s = Stream {
            reader,
            pos: 0,
            first: Header::default(),
            audio_at: 0,
            next_index: 0,
            index: Vec::new(),
        };
        s.seek_to(0)?;
        let mut id3 = [0u8; 10];
        let got = s.read_up_to(&mut id3)?;
        let mut at = id3v2_length(&id3[..got]).unwrap_or(0) as u64;

        // The first frame, confirmed by the one after it. One window, scanned:
        // the confirmation may fall past the window's end, and then it is read
        // from the file.
        s.seek_to(at)?;
        let mut window = vec![0u8; OPEN_SEARCH_LIMIT + MAX_FRAME_BYTES + 2 * HEADER_WINDOW];
        let got = s.read_up_to(&mut window)?;
        let window = &window[..got];
        let mut found = None;
        for moved in 0..=OPEN_SEARCH_LIMIT {
            if moved + 7 > got {
                break;
            }
            let Some(here) = Header::parse(&window[moved..]) else {
                continue;
            };
            let after = moved + here.frame_bytes as usize;
            let next = if after + 7 <= got {
                Header::parse(&window[after..])
            } else {
                let mut h = [0u8; HEADER_WINDOW];
                s.seek_to(at + after as u64)?;
                let n = s.read_up_to(&mut h)?;
                Header::parse(&h[..n])
            };
            if next.is_some_and(|next| here.same_stream(&next)) {
                found = Some((here, moved as u64));
                break;
            }
        }
        let Some((first, moved)) = found else {
            return Ok(None);
        };
        at += moved;
        s.first = first;
        s.audio_at = at;
        s.index.push(at);
        s.seek_to(at)?;
        Ok(Some(s))
    }

    /// The first frame's header, which is the stream's configuration.
    pub fn header(&self) -> &Header {
        &self.first
    }

    /// The next raw_data_block into `dst`. **The header is the container's
    /// and does not go to the codec.** What comes out is exactly what an MP4
    /// sample is, which is what makes the two containers interchangeable to it.
    pub fn read_packet(&mut self, dst: &mut [u8]) -> io::Result<Packet> {
        let Some((header, at)) = self.next_header()? else {
            return Ok(Packet::End);
        };
        let payload = (header.frame_bytes - header.header_bytes) as usize;
        if dst.len() < payload {
            self.seek_to(at)?;
            return Ok(Packet::TooSmall(payload));
        }
        self.seek_to(at + u64::from(header.header_bytes))?;
        if self.read_up_to(&mut dst[..payload])? != payload {
            return Ok(Packet::End); // a truncated last frame is the end of the audio
        }

        let frame = self.next_index * FRAME_SAMPLES;
        self.next_index += 1;
        if self.next_index.is_multiple_of(INDEX_STRIDE) {
            let bucket = (self.next_index / INDEX_STRIDE) as usize;
            if self.index.len() == bucket {
                self.index.push(at + u64::from(header.frame_bytes));
            }
        }
        Ok(Packet::Frame {
            bytes: payload,
            frame,
        })
    }

    /// To the frame before the one holding `frame`: AAC's MDCT windows
    /// overlap by half, so a decoder started cold on the target frame is
    /// wrong for its first output, and the host discards that pre-roll.
    pub fn seek(&mut self, frame: u64) -> io::Result<()> {
        let wanted = frame / FRAME_SAMPLES;
        let start = wanted.saturating_sub(1);
        let Some(at) = self.offset_of(start)? else {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "the stream ends before that frame",
            ));
        };
        self.seek_to(at)?;
        self.next_index = start;
        Ok(())
    }

    fn seek_to(&mut self, at: u64) -> io::Result<()> {
        if self.pos != at {
            self.reader.seek(SeekFrom::Start(at))?;
            self.pos = at;
        }
        Ok(())
    }

    /// Fills as much of `buf` as the reader has, and says how much that was.
    fn read_up_to(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut got = 0usize;
        while got < buf.len() {
            match self.reader.read(&mut buf[got..]) {
                Ok(0) => break,
                Ok(n) => got += n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => {
                    // Where the reader is now is anyone's guess; the next read
                    // will seek first.
                    self.pos = u64::MAX;
                    return Err(e);
                }
            }
        }
        self.pos += got as u64;
        Ok(got)
    }

    /// The next frame header at the current position, stepping over anything
    /// that is not one. None at the end of the audio.
    fn next_header(&mut self) -> io::Result<Option<(Header, u64)>> {
        let at = self.pos;
        let mut h = [0u8; HEADER_WINDOW];
        let got = self.read_up_to(&mut h)?;
        if got < 7 {
            return Ok(None);
        }
        if let Some(header) = Header::parse(&h[..got]) {
            if header.same_stream(&self.first) {
                return Ok(Some((header, at)));
            }
        }
        // Resynchronise: one window from the byte after, scanned for the next
        // header that belongs to this stream.
        self.seek_to(at + 1)?;
        let mut window = vec![0u8; RESYNC_LIMIT + HEADER_WINDOW];
        let got = self.read_up_to(&mut window)?;
        for moved in 0..RESYNC_LIMIT {
            if moved + 7 > got {
                break;
            }
            if let Some(header) = Header::parse(&window[moved..got]) {
                if header.same_stream(&self.first) {
                    let found = at + 1 + moved as u64;
                    self.seek_to(found + HEADER_WINDOW as u64)?;
                    return Ok(Some((header, found)));
                }
            }
        }
        Ok(None)
    }

    /// The byte offset of frame `target`, walking and indexing as far as it
    /// must. None when the stream ends first.
    fn offset_of(&mut self, target: u64) -> io::Result<Option<u64>> {
        let bucket = (target / INDEX_STRIDE) as usize;
        let have = if bucket < self.index.len() {
            bucket
        } else {
            self.index.len() - 1
        };
        let mut at = self.index[have];
        let mut frame = have as u64 * INDEX_STRIDE;
        self.seek_to(at)?;
        while frame < target {
            let Some((header, here)) = self.next_header()? else {
                return Ok(None);
            };
            at = here + u64::from(header.frame_bytes);
            frame += 1;
            if frame.is_multiple_of(INDEX_STRIDE) {
                let next_bucket = (frame / INDEX_STRIDE) as usize;
                if self.index.len() == next_bucket {
                    self.index.push(at);
                }
            }
            self.seek_to(at)?;
        }
        Ok(Some(at))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// A seven-byte header for AAC-LC, 44.1 kHz, stereo, a frame of `bytes`.
    fn header(bytes: u32) -> [u8; 7] {
        let mut h = [0xFF, 0xF1, 0x50, 0x80, 0x00, 0x1F, 0xFC];
        h[3] |= ((bytes >> 11) & 0x3) as u8;
        h[4] = ((bytes >> 3) & 0xFF) as u8;
        h[5] = (((bytes & 0x7) << 5) as u8) | 0x1F;
        h
    }

    /// `n` frames of `payload` bytes each, every frame's payload filled with
    /// its own index so a packet can be told from its neighbours.
    fn stream(n: usize, payload: usize) -> Vec<u8> {
        let mut out = Vec::new();
        for i in 0..n {
            out.extend_from_slice(&header((7 + payload) as u32));
            out.extend(std::iter::repeat_n(i as u8, payload));
        }
        out
    }

    #[test]
    fn a_header_parses_into_the_fields_it_encodes() {
        let h = Header::parse(&header(300)).unwrap();
        assert_eq!(h.object_type, 2);
        assert_eq!(h.rate_index, 4);
        assert_eq!(h.sample_rate(), 44100);
        assert_eq!(h.channel_config, 2);
        assert_eq!(h.header_bytes, 7);
        assert_eq!(h.frame_bytes, 300);
        // AAC-LC, 44.1 kHz, stereo -- the same two bytes an MP4 would carry.
        assert_eq!(h.audio_specific_config(), [0x12, 0x10]);
    }

    #[test]
    fn a_frame_that_cannot_hold_its_own_header_is_not_a_frame() {
        assert!(Header::parse(&header(7)).is_none());
        assert!(Header::parse(&header(0)).is_none());
        assert!(Header::parse(&header(300)[..6]).is_none());
    }

    #[test]
    fn one_sync_pattern_is_not_a_stream() {
        let mut lone = vec![0u8; 64];
        lone[..7].copy_from_slice(&header(300));
        assert_eq!(probe(&lone), 0);
        assert_eq!(probe(&stream(2, 40)), 100);
        // Behind a tag the probe cannot see the end of: nothing, on purpose.
        let mut tagged = b"ID3\x04\x00\x00\x00\x00\x7F\x00".to_vec();
        tagged.extend_from_slice(&stream(2, 40));
        assert_eq!(probe(&tagged), 0);
    }

    #[test]
    fn the_packets_come_out_in_order_without_their_headers() {
        let mut s = Stream::open(Cursor::new(stream(5, 40))).unwrap().unwrap();
        let mut buf = [0u8; 64];
        for i in 0..5u8 {
            let got = s.read_packet(&mut buf).unwrap();
            assert_eq!(
                got,
                Packet::Frame {
                    bytes: 40,
                    frame: u64::from(i) * FRAME_SAMPLES
                }
            );
            assert!(buf[..40].iter().all(|&b| b == i));
        }
        assert_eq!(s.read_packet(&mut buf).unwrap(), Packet::End);
    }

    #[test]
    fn a_small_buffer_consumes_nothing() {
        let mut s = Stream::open(Cursor::new(stream(2, 40))).unwrap().unwrap();
        let mut small = [0u8; 8];
        assert_eq!(s.read_packet(&mut small).unwrap(), Packet::TooSmall(40));
        let mut buf = [0u8; 64];
        assert_eq!(
            s.read_packet(&mut buf).unwrap(),
            Packet::Frame {
                bytes: 40,
                frame: 0
            }
        );
    }

    #[test]
    fn junk_between_frames_is_stepped_over() {
        // Two frames to open on -- a frame is only believed when the one after
        // it agrees -- then a sync that agrees with nothing, some bytes, and
        // two more frames.
        let mut bytes = stream(2, 40);
        bytes.extend_from_slice(&[0xFF, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00]);
        bytes.extend_from_slice(&[1, 2, 3, 4, 5]);
        bytes.extend_from_slice(&stream(2, 40));
        let mut s = Stream::open(Cursor::new(bytes)).unwrap().unwrap();
        let mut buf = [0u8; 64];
        for expect in [0u8, 1, 0, 1] {
            assert!(matches!(
                s.read_packet(&mut buf).unwrap(),
                Packet::Frame { bytes: 40, .. }
            ));
            assert!(buf[..40].iter().all(|&b| b == expect));
        }
        assert_eq!(s.read_packet(&mut buf).unwrap(), Packet::End);
    }

    #[test]
    fn a_lone_first_frame_is_not_enough_to_open_on() {
        // One header and nothing after it that agrees is what a chance sync in
        // somebody else's file looks like.
        assert!(Stream::open(Cursor::new(stream(1, 40))).unwrap().is_none());
    }

    #[test]
    fn a_seek_lands_one_frame_early_for_the_pre_roll() {
        let mut s = Stream::open(Cursor::new(stream(100, 40))).unwrap().unwrap();
        let mut buf = [0u8; 64];
        s.seek(70 * FRAME_SAMPLES + 5).unwrap();
        assert_eq!(
            s.read_packet(&mut buf).unwrap(),
            Packet::Frame {
                bytes: 40,
                frame: 69 * FRAME_SAMPLES
            }
        );
        assert!(buf[..40].iter().all(|&b| b == 69));
        // And frame 0 has nothing before it.
        s.seek(0).unwrap();
        assert!(matches!(
            s.read_packet(&mut buf).unwrap(),
            Packet::Frame { frame: 0, .. }
        ));
        // Past the end is an error, not a guess.
        assert!(s.seek(1000 * FRAME_SAMPLES).is_err());
    }

    #[test]
    fn not_adts_at_all_is_declined_not_guessed() {
        assert!(Stream::open(Cursor::new(vec![0u8; 4096]))
            .unwrap()
            .is_none());
        assert!(Stream::open(Cursor::new(Vec::<u8>::new()))
            .unwrap()
            .is_none());
    }
}

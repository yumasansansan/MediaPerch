// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSD over PCM: the packing, and nothing else.
//
// **DoP is not a conversion and this is not a decoder.** A DSD DAC reached over
// WASAPI cannot be handed a bitstream -- Windows has no DSD wire format -- so
// the bits travel inside 24-bit PCM frames under a marker the DAC recognises.
// Every DSD bit arrives, in order, unaltered; what changes is the framing.
// That is the whole of the format, and it is why this belongs beside a decoder
// rather than inside the graph: the graph's repack keeps the frame count and
// changes the container, and this halves the frame count.
//
// One 24-bit frame per channel carries two DSD bytes:
//
//   bits 23..16   the marker: 0x05, then 0xFA, alternating every frame
//   bits 15..8    the earlier DSD byte
//   bits  7..0    the later one
//
// The marker is what makes the stream self-describing. A DAC that understands
// DoP sees 0x05/0xFA alternating and switches to DSD; one that does not sees
// PCM whose top byte never settles, which is quiet noise rather than full-scale
// noise, and that is the reason the marker was chosen to be those two values.
// **All channels of one frame carry the same marker**, and the alternation is
// per frame rather than per sample, which is the detail a reader of the spec
// gets wrong first.

#![forbid(unsafe_code)]

/// The two marker bytes, in the order they are written.
pub const MARKER_A: u8 = 0x05;
pub const MARKER_B: u8 = 0xFA;

/// DSD bytes consumed for one DoP frame of one channel.
pub const DSD_BYTES_PER_FRAME: usize = 2;

/// Bytes one DoP frame of one channel occupies: a packed 24-bit sample.
pub const DOP_FRAME_BYTES: usize = 3;

/// Where in the two-frame marker cycle the next frame sits.
///
/// Carried by the caller across packets: the alternation is a property of the
/// stream, not of a buffer, and restarting it every packet would put two 0x05
/// frames next to each other at every packet boundary -- which a DAC reads as a
/// stream it has lost, at exactly the rate packets arrive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Phase(bool);

impl Phase {
    /// The phase a stream starts on, and what a seek returns it to.
    ///
    /// **A seek may start on either marker and the audio is the same**, because
    /// the marker carries no audio and the DAC locks to the alternation rather
    /// than to a position in it. Starting from a fixed phase is what makes two
    /// decodes of the same file identical, which is what the tests compare.
    pub fn start() -> Phase {
        Phase(false)
    }

    fn marker(self) -> u8 {
        if self.0 {
            MARKER_B
        } else {
            MARKER_A
        }
    }

    fn next(self) -> Phase {
        Phase(!self.0)
    }
}

/// Bytes `pack` writes for `dsd_bytes` of input.
///
/// An odd byte count cannot be packed -- a DoP frame is two DSD bytes and there
/// is no half a frame -- so the answer is what the whole frames need.
pub fn packed_bytes(dsd_bytes: usize, channels: usize) -> usize {
    if channels == 0 {
        return 0;
    }
    let per_channel = dsd_bytes / channels;
    (per_channel / DSD_BYTES_PER_FRAME) * channels * DOP_FRAME_BYTES
}

/// Packs byte-interleaved MSB-first DSD into interleaved 24-bit DoP frames.
///
/// `src` is `n * channels` bytes, channel-interleaved. `dst` receives
/// `packed_bytes(src.len(), channels)`. Returns the bytes written, and advances
/// `phase` by the frames it wrote.
///
/// A trailing odd DSD byte per channel is **not** consumed and not written: the
/// caller keeps it for the next packet, or drops it at the end of the file,
/// where one leftover byte is 0.35 microseconds of audio at DSD64.
pub fn pack(src: &[u8], channels: usize, dst: &mut [u8], phase: &mut Phase) -> usize {
    if channels == 0 {
        return 0;
    }
    let frames = (src.len() / channels) / DSD_BYTES_PER_FRAME;
    let out_bytes = frames * channels * DOP_FRAME_BYTES;
    if dst.len() < out_bytes {
        return 0;
    }
    for f in 0..frames {
        let marker = phase.marker();
        for c in 0..channels {
            // The two DSD bytes of this channel, in time order. The source is
            // interleaved, so consecutive bytes of one channel are `channels`
            // apart.
            let first = src[(f * DSD_BYTES_PER_FRAME) * channels + c];
            let second = src[(f * DSD_BYTES_PER_FRAME + 1) * channels + c];
            // Packed 24-bit, little-endian: the least significant byte first,
            // so the marker lands last. `repack` documents the same convention
            // for every other container here.
            let at = (f * channels + c) * DOP_FRAME_BYTES;
            dst[at] = second;
            dst[at + 1] = first;
            dst[at + 2] = marker;
        }
        *phase = phase.next();
    }
    out_bytes
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_frame_carries_two_dsd_bytes_under_a_marker() {
        let src = [0xAA, 0xBB]; // mono: two bytes, one frame
        let mut dst = [0u8; 3];
        let mut phase = Phase::start();
        assert_eq!(pack(&src, 1, &mut dst, &mut phase), 3);
        // Little-endian packed 24-bit: low byte first, marker highest.
        assert_eq!(dst, [0xBB, 0xAA, MARKER_A]);
    }

    #[test]
    fn the_marker_alternates_every_frame_and_not_every_sample() {
        // Stereo, four DSD bytes per channel: two frames.
        let src = [1, 101, 2, 102, 3, 103, 4, 104];
        let mut dst = [0u8; 12];
        let mut phase = Phase::start();
        assert_eq!(pack(&src, 2, &mut dst, &mut phase), 12);
        // Frame 0: both channels marked 0x05.
        assert_eq!(dst[2], MARKER_A);
        assert_eq!(dst[5], MARKER_A);
        // Frame 1: both marked 0xFA.
        assert_eq!(dst[8], MARKER_B);
        assert_eq!(dst[11], MARKER_B);
    }

    #[test]
    fn the_phase_carries_across_packets() {
        let src = [1, 2]; // mono, one frame
        let mut dst = [0u8; 3];
        let mut phase = Phase::start();
        pack(&src, 1, &mut dst, &mut phase);
        assert_eq!(dst[2], MARKER_A);
        // The next packet must continue the alternation, not restart it.
        pack(&src, 1, &mut dst, &mut phase);
        assert_eq!(dst[2], MARKER_B);
        pack(&src, 1, &mut dst, &mut phase);
        assert_eq!(dst[2], MARKER_A);
    }

    #[test]
    fn every_dsd_bit_survives_in_order() {
        // The claim the whole format rests on, checked by taking the bits back
        // out: this is the test that would fail if the two bytes were swapped,
        // which is the mistake that sounds like noise rather than like nothing.
        let src: Vec<u8> = (0..64u32).map(|i| (i * 37 + 11) as u8).collect();
        let mut dst = vec![0u8; packed_bytes(src.len(), 2)];
        let mut phase = Phase::start();
        assert_eq!(pack(&src, 2, &mut dst, &mut phase), dst.len());

        let mut back = Vec::new();
        for frame in dst.chunks_exact(2 * DOP_FRAME_BYTES) {
            // Channel 0 then channel 1, each contributing its earlier byte;
            // then the same for the later byte, which is the interleaving the
            // source had.
            for half in 0..2 {
                for c in 0..2 {
                    let s = &frame[c * DOP_FRAME_BYTES..];
                    back.push(if half == 0 { s[1] } else { s[0] });
                }
            }
        }
        assert_eq!(back, src);
    }

    #[test]
    fn an_odd_byte_is_left_for_the_next_packet_rather_than_half_framed() {
        let src = [1, 2, 3]; // mono: one whole frame and a byte over
        assert_eq!(packed_bytes(src.len(), 1), 3);
        let mut dst = [0u8; 3];
        let mut phase = Phase::start();
        assert_eq!(pack(&src, 1, &mut dst, &mut phase), 3);
        assert_eq!(dst, [2, 1, MARKER_A]);
    }

    #[test]
    fn a_buffer_that_is_too_small_writes_nothing() {
        let src = [1, 2, 3, 4];
        let mut dst = [0u8; 5]; // needs 6
        let mut phase = Phase::start();
        assert_eq!(pack(&src, 1, &mut dst, &mut phase), 0);
        assert_eq!(dst, [0; 5]);
        assert_eq!(phase, Phase::start());
    }

    #[test]
    fn nothing_in_nothing_out() {
        let mut dst = [0u8; 8];
        let mut phase = Phase::start();
        assert_eq!(pack(&[], 2, &mut dst, &mut phase), 0);
        assert_eq!(pack(&[1, 2], 0, &mut dst, &mut phase), 0);
        assert_eq!(packed_bytes(0, 2), 0);
        assert_eq!(packed_bytes(8, 0), 0);
    }
}

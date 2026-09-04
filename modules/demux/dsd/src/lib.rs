// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSD files -- Sony's DSF and Philips' DSDIFF -- as a container.
//
// The reading is the `dsdfile` crate in reader/, which walks either wrapper and
// hands back byte-interleaved MSB-first DSD; this file opens the file and
// answers the ABI's questions about the one stream. Both carry `MP_CODEC_DSD`.
//
// **Everything this module reports is in DoP frames, not DSD bytes**, and that
// is not a matter of taste. `MpStreamInfo::total_frames`, `MpPacket::frame` and
// the argument to `seek` are all in the units the *codec* produces -- the host
// subtracts one from the other to decide how much of a decoded packet to
// discard after a seek -- and `codec_dsd` produces DoP, whose frames are two
// DSD bytes each. A container that reported bytes would give a track twice its
// length and land every seek at half its target.
//
// So the arithmetic, once, here: DSD64 is 2822400 DSD samples a second, which
// is 352800 bytes, which is **176400 DoP frames**. DSD256 is 705600, which is
// the number docs/plan.md §5 records a FiiO KA5 accepting. DSD512 would be
// 1411200, which it does not, and that is what a native-DSD sink would be for.
//
// **Every `unsafe` this module needs is in `mp-abi`, and this file has none.**

#![deny(unsafe_code)]

use std::fs::File;
use std::io::BufReader;

use mp_abi::{
    codec, encoding, level, packet_flag, sample, stream_flag, stream_kind, Demux, Error, Format,
    Next, StreamInfo,
};

/// DSD bytes in one DoP frame, per channel.
const BYTES_PER_DOP_FRAME: u64 = 2;

pub struct DsdDemux {
    reader: dsdfile::Reader<BufReader<File>>,
    /// What `codec_dsd` is told: the rate, the channels and the layout. See
    /// `config` below for why this exists at all.
    config: [u8; 9],
    format: Format,
    /// One packet, read from the file before it is copied out. The reader wants
    /// somewhere to weave DSF's blocks into, and the ABI's `dst` may be too
    /// small, in which case nothing may be consumed.
    scratch: Vec<u8>,
}

/// The configuration blob, which is this tree's own because DSD has no
/// equivalent of an `AudioSpecificConfig`.
///
/// A codec module never sees a file and is handed only this, so everything it
/// needs to state its output format has to be in here: the DSD rate, the
/// channel count and the speaker mask. Nine bytes, little-endian, and the same
/// nine whichever wrapper the container was -- which is the point, exactly as
/// it is for `demux_adts` assembling the two bytes an MP4 would have carried.
fn config_bytes(rate: u32, channels: u32, mask: u32) -> [u8; 9] {
    let mut out = [0u8; 9];
    out[..4].copy_from_slice(&rate.to_le_bytes());
    out[4] = channels as u8;
    out[5..].copy_from_slice(&mask.to_le_bytes());
    out
}

impl Demux for DsdDemux {
    fn probe(_path: &str, head: &[u8]) -> u32 {
        dsdfile::probe(head)
    }

    fn open(path: &str) -> Result<Self, Error> {
        let file = File::open(path).map_err(|_| Error::Io)?;
        let reader = match dsdfile::Reader::open(BufReader::with_capacity(1 << 16, file)) {
            Ok(Ok(reader)) => reader,
            Ok(Err(why)) => {
                mp_abi::log(level::DEBUG, &format!("{path}: {why}"));
                return Err(Error::Unsupported);
            }
            Err(_) => return Err(Error::Io),
        };
        let info = *reader.info();
        mp_abi::log(
            level::DEBUG,
            &format!(
                "{}: DSD{}, {} Hz, {} channels",
                info.wrapper.name(),
                info.dsd_rate / 44100,
                info.dsd_rate,
                info.channels
            ),
        );
        // The rate the codec will produce, for the reason at the top of this
        // file: two DSD bytes to a DoP frame.
        let dop_rate = info.byte_rate() / BYTES_PER_DOP_FRAME as u32;
        let format = Format {
            sample_rate: dop_rate,
            channels: info.channels,
            channel_mask: info.channel_mask,
            // **The sample type is the codec's**, as it is for AAC: what the
            // container holds is a bitstream, and what comes out is 24-bit
            // frames, and only the codec is in a position to say so.
            sample_type: sample::NONE,
            encoding: encoding::DOP,
            valid_bits: 0,
            reserved: [0; 2],
        };
        let scratch = vec![0u8; reader.max_packet_bytes()];
        Ok(DsdDemux {
            reader,
            config: config_bytes(info.dsd_rate, info.channels, info.channel_mask),
            format,
            scratch,
        })
    }

    fn stream_count(&self) -> u32 {
        1
    }

    fn stream_info(&self, index: u32) -> Result<StreamInfo, Error> {
        if index != 0 {
            return Err(Error::Invalid);
        }
        // **No edit, and there is nothing to be sorry about.** DSD has no
        // encoder delay to discard: there is no transform, no window and no
        // priming frame, so the first byte of the file is the first byte of the
        // audio. `total_frames` is exact rather than a claim, because it is the
        // bytes that are there divided by two.
        Ok(StreamInfo {
            kind: stream_kind::AUDIO,
            codec: codec::DSD,
            flags: stream_flag::DEFAULT,
            config_bytes: self.config.len() as u32,
            format: self.format,
            total_frames: self.reader.info().total_bytes_per_channel / BYTES_PER_DOP_FRAME,
            ..StreamInfo::default()
        })
    }

    fn stream_config(&self, index: u32) -> Result<&[u8], Error> {
        if index != 0 {
            return Err(Error::Invalid);
        }
        Ok(&self.config)
    }


    fn read_packet(&mut self, dst: &mut [u8]) -> Result<Next, Error> {
        // Read into the scratch first: the ABI says a packet that does not fit
        // consumes nothing, and the reader has already moved by the time it
        // could be measured.
        match self.reader.read_packet(&mut self.scratch) {
            Ok(Some((bytes, first_dsd_byte))) => {
                if bytes == 0 {
                    return Ok(Next::TooSmall(self.scratch.len()));
                }
                if dst.len() < bytes {
                    // Nothing was consumed from the caller's point of view: the
                    // bytes are in the scratch and the next call re-reads them
                    // from the file, because `read_packet` above has already
                    // advanced. Rewind so it does.
                    let _ = self.reader.seek(first_dsd_byte);
                    return Ok(Next::TooSmall(bytes));
                }
                dst[..bytes].copy_from_slice(&self.scratch[..bytes]);
                Ok(Next::Packet {
                    bytes,
                    frame: first_dsd_byte / BYTES_PER_DOP_FRAME,
                    // **Every packet is a sync point, which no other codec
                    // here can say.** A DSD byte depends on nothing before it:
                    // no predictor, no lapped transform, no bit reservoir. So
                    // the host may start decoding at any packet and needs no
                    // pre-roll -- the one frame `seek` rounds down by is about
                    // DoP's marker and not about the audio, which would be the
                    // same either way.
                    flags: packet_flag::SYNC | packet_flag::TIMED,
                })
            }
            Ok(None) => Ok(Next::End),
            Err(_) => Err(Error::Io),
        }
    }

    fn seek(&mut self, stream: u32, frame: u64) -> Result<(), Error> {
        if stream != 0 {
            return Err(Error::Invalid); // one stream, and it is 0
        }
        // **Down to an even frame, and the reason is the marker.**
        //
        // DoP's marker alternates 0x05, 0xFA every frame, so which one a frame
        // carries is decided by whether it is even. `codec_dsd` is reset before
        // a seek and starts on 0x05, so landing on an odd frame gives every
        // frame after it the wrong marker -- measured, before this line existed:
        // seeks to 2 and 1000 were byte-identical to a straight decode and
        // seeks to 1, 8191 and 17639 were not, which is as clean a statement of
        // "the parity is wrong" as a test can make.
        //
        // Landing one frame early costs the host one frame to discard, which is
        // what `MpDemuxVtbl::seek` says it does with everything before the
        // target anyway. It is the same shape as the pre-roll AAC needs, for a
        // different reason and a thousand times smaller.
        self.reader
            .seek((frame & !1u64) * BYTES_PER_DOP_FRAME)
            .map_err(|_| Error::Io)
    }
}

mp_abi::export_demux!(
    DsdDemux,
    id: "demux_dsd",
    name: "DSD files: DSF and DSDIFF (in Rust)",
    version: (0, 1, 0),
    priority: 110,
    codecs: [codec::DSD]
);

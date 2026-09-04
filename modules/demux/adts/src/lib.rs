// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raw AAC in ADTS framing, as a container -- and the first container module in
// this tree that is not C++.
//
// **This is the module the v2 plan forgot.** The plan's table turned
// `decode_aac` into `codec_aac` and said its MP4 half went to `demux_mp4` --
// which is true, and left out that `decode_aac` had a *third* part: the ADTS
// framer. A raw `.aac` file is not an MP4 and not an MPEG audio stream, and
// without this it would have gone from a format with a first-class reader to
// one only FFmpeg could open.
//
// The framing is the `adts` crate in framer/, which walks any seekable reader
// and carries `#![forbid(unsafe_code)]`; this file opens the file, answers the
// ABI's questions about the one stream, and maps the framer's answers onto the
// header's contract. `std::fs::File` takes the UTF-8 path the host hands over
// and spells it however the OS wants, which is the Windows Unicode conversion
// every C++ demuxer here carries by hand, done once by the standard library.
//
// **Every `unsafe` this module needs is in `mp-abi`, and this file has none.**

#![deny(unsafe_code)]

use std::fs::File;
use std::io::BufReader;

use mp_abi::{
    codec, encoding, level, packet_flag, sample, stream_flag, stream_kind, Demux, Error, Format,
    Next, StreamInfo,
};

pub struct AdtsDemux {
    stream: adts::Stream<BufReader<File>>,
    /// The AudioSpecificConfig, assembled from the first header.
    config: [u8; 2],
    format: Format,
}

impl Demux for AdtsDemux {
    fn probe(_path: &str, head: &[u8]) -> u32 {
        adts::probe(head)
    }

    fn open(path: &str) -> Result<Self, Error> {
        let file = File::open(path).map_err(|_| Error::Io)?;
        let stream = match adts::Stream::open(BufReader::with_capacity(1 << 16, file)) {
            Ok(Some(stream)) => stream,
            Ok(None) => {
                mp_abi::log(
                    level::DEBUG,
                    &format!("{path}: no ADTS frame this demuxer can confirm"),
                );
                return Err(Error::Unsupported);
            }
            Err(_) => return Err(Error::Io),
        };
        let header = *stream.header();
        // **The sample type is the codec's**, and so is the channel layout:
        // channel configuration 0 puts the layout in a program config element
        // inside the first frame, which is a thing only a decoder can read.
        // Where the header does state a count this reports it, which is every
        // real file.
        let format = Format {
            sample_rate: header.sample_rate(),
            channels: header.channel_config,
            channel_mask: 0,
            sample_type: sample::NONE,
            encoding: encoding::PCM,
            valid_bits: 0,
            reserved: [0; 2],
        };
        Ok(AdtsDemux {
            stream,
            config: header.audio_specific_config(),
            format,
        })
    }

    fn stream_count(&self) -> u32 {
        1
    }

    fn stream_info(&self, index: u32) -> Result<StreamInfo, Error> {
        if index != 0 {
            return Err(Error::Invalid);
        }
        // **No length and no edit, because a raw stream states neither.** An
        // MP4 carries `elst` and a frame count; ADTS carries nothing but
        // frames, so zero here says "the container did not say" rather than
        // "there is none".
        Ok(StreamInfo {
            kind: stream_kind::AUDIO,
            codec: codec::AAC_LC,
            flags: stream_flag::DEFAULT,
            config_bytes: self.config.len() as u32,
            format: self.format,
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
        match self.stream.read_packet(dst) {
            // AAC frames overlap by half a window, so no frame is a sync point
            // on its own; `seek` hands back the one before as pre-roll and the
            // host drops it.
            Ok(adts::Packet::Frame { bytes, frame }) => Ok(Next::Packet {
                bytes,
                frame,
                flags: packet_flag::TIMED,
            }),
            Ok(adts::Packet::End) => Ok(Next::End),
            Ok(adts::Packet::TooSmall(needed)) => Ok(Next::TooSmall(needed)),
            Err(_) => Err(Error::Io),
        }
    }

    fn seek(&mut self, stream: u32, frame: u64) -> Result<(), Error> {
        if stream != 0 {
            return Err(Error::Invalid); // one stream, and it is 0
        }
        self.stream.seek(frame).map_err(|_| Error::Io)
    }
}

mp_abi::export_demux!(
    AdtsDemux,
    id: "demux_adts",
    name: "AAC in ADTS framing (the frame headers, in Rust)",
    version: (0, 2, 0),
    priority: 108,
    codecs: [codec::AAC_LC]
);

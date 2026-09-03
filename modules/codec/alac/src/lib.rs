// SPDX-License-Identifier: GPL-3.0-or-later
//
// ALAC, as a codec and nothing else -- and the first module in this tree that
// is not C++.
//
// The reasoning that put it in Rust is the reasoning that put it here at all,
// carried one step further. It was written rather than linked because Apple's
// reference is the specification and has been unmaintained since 2011, and the
// five things that reference does not check (docs/formats.md has the table)
// are all reads or writes past a buffer that a crafted packet controls. In C++
// each of those was a check somebody had to think of; a fuzzer found the class
// of bug the same kind of code has, in the FLAC parser that used to be beside
// this one, in ninety seconds. In Rust the class does not exist: an index past
// a slice is a panic, the panic is caught at the module boundary, and the host
// sees a packet that failed to decode -- which is what it was.
//
// **Every `unsafe` this module needs is in `mp-abi`, and this file has none.**
// The decoder is its own crate, `decoder/`, and carries `#![forbid(unsafe_code)]`
// at crate level, which the compiler enforces. This file is `deny`, with a
// single `allow` inside the `export_codec!` expansion for the one `#[no_mangle]`
// symbol a module has to export.
//
// What this module does is what `codec_alac.cpp` did: never see a file, take
// the `ALACSpecificConfig` the container found and then packets one at a time,
// and produce the file's own integers. Two things live here because they are
// properties of ALAC rather than of any container: the channel order, which is
// Apple's and not WAVE's, and the depth, which the magic cookie states and the
// container does not.

#![deny(unsafe_code)]

use mp_abi::{codec, encoding, level, sample, Codec, Error, Format};

/// Bytes per sample for a depth, which is the smallest container that holds
/// it: 16 → 2, 20 and 24 → 3, 32 → 4.
fn container_for(bits: u32) -> u32 {
    if bits <= 16 {
        2
    } else if bits <= 24 {
        3
    } else {
        4
    }
}

fn sample_type_for(container: u32, valid: u32) -> u32 {
    match container {
        2 => sample::S16,
        3 => sample::S24_PACKED,
        _ => {
            if valid <= 24 {
                sample::S24_IN_32
            } else {
                sample::S32
            }
        }
    }
}

pub struct AlacCodec {
    decoder: alac::Decoder,
    format: Format,
    container: u32,
    shift: u32,
    /// One frame, interleaved, in ALAC channel order, before the remap.
    decoded: Vec<i32>,
}

impl Codec for AlacCodec {
    fn probe(codec_id: u32, config: &[u8]) -> u32 {
        // **A question about data, not about a file.** The cookie either parses
        // or it does not, and a codec that could not read this stream should
        // say so before the host commits to it rather than after.
        if codec_id == codec::ALAC && alac::Config::parse(config).is_ok() {
            100
        } else {
            0
        }
    }

    fn open(codec_id: u32, config: &[u8]) -> Result<Self, Error> {
        if codec_id != codec::ALAC {
            return Err(Error::Unsupported);
        }
        let cfg = match alac::Config::parse(config) {
            Ok(cfg) => cfg,
            Err(why) => {
                mp_abi::log(
                    level::DEBUG,
                    &format!(
                        "the container's ALACSpecificConfig is {} bytes and does not parse: {why}",
                        config.len()
                    ),
                );
                return Err(Error::Format);
            }
        };
        let decoder = match alac::Decoder::new(&cfg) {
            Ok(d) => d,
            Err(why) => {
                mp_abi::log(
                    level::WARN,
                    &format!(
                        "ALAC at {} Hz, {} channels, {} bits: the decoder refused it: {why}",
                        cfg.sample_rate, cfg.channels, cfg.bit_depth
                    ),
                );
                return Err(Error::Format);
            }
        };

        // The depth is the cookie's, and the container it goes in is the
        // smallest one that holds it -- left-justified where that leaves room,
        // which is what the ABI means by `valid_bits`.
        let depth = u32::from(cfg.bit_depth);
        let channels = u32::from(cfg.channels);
        let container = container_for(depth);
        let format = Format {
            sample_rate: cfg.sample_rate,
            channels,
            channel_mask: alac::layout_for(cfg.channels).mask,
            sample_type: sample_type_for(container, depth),
            encoding: encoding::PCM,
            valid_bits: depth,
            reserved: [0; 2],
        };
        Ok(AlacCodec {
            decoder,
            format,
            container,
            shift: container * 8 - depth,
            decoded: vec![0; cfg.frame_length as usize * cfg.channels as usize],
        })
    }

    fn format(&self) -> Format {
        self.format
    }

    fn decode(&mut self, packet: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
        if packet.is_empty() {
            return Err(Error::Invalid);
        }
        let frames = self
            .decoder
            .decode(packet, &mut self.decoded)
            .map_err(|_| Error::Format)? as usize;

        let channels = self.decoder.channels() as usize;
        let container = self.container as usize;
        let needed = frames * channels * container;
        if dst.len() < needed {
            return Err(Error::NoMemory);
        }

        // **Apple's channel order into WAVE's**, which is the one thing about
        // this codec that a decoder can get perfectly right and still put every
        // channel in the wrong speaker. docs/formats.md records what happens
        // when it is skipped: eight of eight channels exact, none of them where
        // they belong.
        let layout = alac::layout_for(self.decoder.channels());
        let mut out = dst[..needed].chunks_exact_mut(container);
        for frame in self.decoded[..frames * channels].chunks_exact(channels) {
            for &from in &layout.from[..channels] {
                let v = (frame[usize::from(from)] as u32) << self.shift;
                let bytes = v.to_le_bytes();
                if let Some(slot) = out.next() {
                    slot.copy_from_slice(&bytes[..container]);
                }
            }
        }
        Ok(needed)
    }

    fn flush(&mut self, _dst: &mut [u8]) -> Result<usize, Error> {
        // ALAC is a packet in, a packet out. Nothing is held back, so there is
        // nothing to give back.
        Ok(0)
    }

    fn reset(&mut self) -> Result<(), Error> {
        // Every ALAC packet is decodable on its own -- there is no inter-packet
        // state to forget, which is why a seek in ALAC needs no pre-roll and a
        // seek in AAC does.
        Ok(())
    }
}

mp_abi::export_codec!(
    AlacCodec,
    id: "codec_alac",
    name: "ALAC (written here, in Rust)",
    version: (0, 2, 0),
    priority: 115,
    codecs: [codec::ALAC]
);

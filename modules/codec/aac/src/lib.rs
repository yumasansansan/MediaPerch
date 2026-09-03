// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC-LC, as a codec and nothing else -- in Rust, after ALAC.
//
// Handed the `AudioSpecificConfig` the container found and then packets, one
// raw_data_block at a time, it produces float -- which is what the codec makes
// and what every lossy decoder here reports, so a file it reads goes to Path B
// and the reason is written down rather than inferred.
//
// What is *not* here is what the module this descends from also carried: half
// an MP4 parser, an ADTS framer, and a file handle. The first is `demux_mp4`'s
// and the second is `demux_adts`'s, and this file cannot tell which of them
// called it, which is the whole point.
//
// **Every `unsafe` this module needs is in `mp-abi`, and this file has none.**
// The decoder is its own crate, `decoder/`, and carries `#![forbid(unsafe_code)]`
// at crate level. This file is `deny`, with a single `allow` inside the
// `export_codec!` expansion for the one `#[no_mangle]` symbol a module has to
// export.

#![deny(unsafe_code)]

use mp_abi::{codec, encoding, level, sample, Codec, Error, Format};

/// The object type an AudioSpecificConfig opens with, read the way the
/// standard spells it: five bits, and 31 means "six more".
fn object_type_of(config: &[u8]) -> u32 {
    let first = config.first().map_or(0, |&b| u32::from(b >> 3));
    if first != 31 {
        return first;
    }
    let low = config.first().map_or(0, |&b| u32::from(b & 0x7));
    let next = config.get(1).map_or(0, |&b| u32::from(b >> 5));
    32 + ((low << 3) | next)
}

pub struct AacCodec {
    decoder: aac::Decoder,
    format: Format,
    /// False until the channel count is known, which for configuration 0 in a
    /// raw stream is after the first frame.
    format_known: bool,
}

impl Codec for AacCodec {
    fn probe(codec_id: u32, config: &[u8]) -> u32 {
        // Object type 2 and nothing else. SBR and PS are a different codec
        // wearing the same name, and `Config::parse` refuses them, which is
        // what lets the host reach `demux_ffmpeg` instead of finding out three
        // packets in.
        if codec_id == codec::AAC_LC && aac::Config::parse(config).is_ok() {
            100
        } else {
            0
        }
    }

    fn open(codec_id: u32, config: &[u8]) -> Result<Self, Error> {
        if codec_id != codec::AAC_LC {
            return Err(Error::Unsupported);
        }
        let cfg = match aac::Config::parse(config) {
            Ok(cfg) => cfg,
            Err(why) => {
                let object_type = object_type_of(config);
                if config.len() >= 2 && object_type != 2 {
                    // **SBR and PS are a different codec wearing the same
                    // name**, and a stream that reaches here with one is a
                    // stream the probe already declined -- so this only
                    // happens when somebody forced the module. Saying which
                    // object type it was is what turns "it did not play" into
                    // something a person can act on.
                    mp_abi::log(
                        level::WARN,
                        &format!(
                            "AAC object type {object_type} is not LC; this module decodes LC \
                             only, and demux_ffmpeg reads the rest"
                        ),
                    );
                    return Err(Error::Unsupported);
                }
                mp_abi::log(
                    level::DEBUG,
                    &format!(
                        "the container's AudioSpecificConfig is {} bytes and does not parse: {why}",
                        config.len()
                    ),
                );
                return Err(Error::Format);
            }
        };
        let decoder = match aac::Decoder::new(&cfg) {
            Ok(d) => d,
            Err(why) => {
                mp_abi::log(
                    level::WARN,
                    &format!(
                        "AAC-LC at {} Hz, {} channels: the decoder refused it: {why}",
                        cfg.sample_rate, cfg.channel_config
                    ),
                );
                return Err(Error::Format);
            }
        };

        let mut format = Format {
            sample_rate: cfg.sample_rate,
            channels: 0,
            channel_mask: 0,
            sample_type: sample::F32,
            encoding: encoding::PCM,
            valid_bits: 0,
            reserved: [0; 2],
        };
        // **The channel count may not be known yet.** Configuration 0 puts the
        // layout in a program config element, which an MP4's
        // AudioSpecificConfig carries and a raw ADTS stream does not -- so for
        // that case the answer arrives with the first frame, and `format` says
        // so by failing until then rather than by guessing stereo.
        let mut format_known = false;
        if cfg.channel_config != 0 {
            let layout = aac::layout_for_config(cfg.channel_config);
            format.channels = cfg.channel_config;
            format.channel_mask = layout.mask;
            format_known = true;
        } else if cfg.pce.count != 0 {
            format.channels = u32::from(cfg.pce.count);
            format.channel_mask = cfg.pce.mask;
            format_known = true;
        }
        Ok(AacCodec {
            decoder,
            format,
            format_known,
        })
    }

    fn format(&self) -> Result<Format, Error> {
        if self.format_known {
            Ok(self.format)
        } else {
            Err(Error::Format)
        }
    }

    fn decode(&mut self, packet: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
        if packet.is_empty() {
            return Err(Error::Invalid);
        }
        self.decoder
            .decode_frame(packet)
            .map_err(|_| Error::Format)?;

        let channels = self.decoder.channels();
        if channels == 0 {
            return Err(Error::Format);
        }
        if !self.format_known {
            // Configuration 0: the first frame carried the program config
            // element, so this is where the layout finally arrives.
            self.format.channels = channels as u32;
            self.format.channel_mask = self.decoder.layout().mask;
            self.format_known = true;
        }

        let needed = aac::FRAME_LEN * channels * std::mem::size_of::<f32>();
        if dst.len() < needed {
            return Err(Error::NoMemory);
        }

        // Planar float into interleaved, in WAVE slot order. FFmpeg's encoder
        // writes configuration 0 for 7.1(wide) and puts the layout in a
        // program config element, so this table is not a corner nobody
        // reaches -- getting it wrong decodes every channel perfectly into the
        // wrong speaker.
        let layout = *self.decoder.layout();
        for (n, frame) in dst[..needed].chunks_exact_mut(channels * 4).enumerate() {
            for (ch, slot) in frame.as_chunks_mut::<4>().0.iter_mut().enumerate() {
                *slot = self.decoder.pcm(usize::from(layout.from[ch]))[n].to_le_bytes();
            }
        }
        Ok(needed)
    }

    fn flush(&mut self, _dst: &mut [u8]) -> Result<usize, Error> {
        // The overlap of the last frame is the encoder's padding, and the
        // container's `play_frames` is what says how much of the audio was
        // real. Emitting it here would be emitting padding the edit exists to
        // remove.
        Ok(0)
    }

    fn reset(&mut self) -> Result<(), Error> {
        // **AAC frames are not independent.** Each one is windowed against the
        // last, so a frame decoded with no predecessor is half a frame of
        // silence overlapped onto real audio. The host feeds the packet before
        // the target and discards it, which is what `reset` exists to make
        // possible: the pre-roll frame is what warms the overlap, so nothing
        // here has to.
        Ok(())
    }
}

mp_abi::export_codec!(
    AacCodec,
    id: "codec_aac",
    name: "AAC-LC (written here, in Rust)",
    version: (0, 2, 0),
    priority: 108,
    codecs: [codec::AAC_LC]
);

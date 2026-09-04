// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSD, as a codec -- which is a word this one has to earn.
//
// **Nothing here decodes.** DSD is already the waveform: a one-bit stream at
// 2.8 MHz and up, which a DAC low-passes into analogue and which no arithmetic
// in this process should ever touch. What this module does is put those bits
// into the frames a Windows endpoint can carry, which is DoP -- two DSD bytes
// in each 24-bit PCM frame, under a marker the DAC recognises. Every bit
// arrives, in order, unaltered.
//
// **So why is it a codec and not a repack?** Because the frame count changes.
// A repack in this tree keeps the frames and moves them between containers --
// that is what `Fidelity::repacked` means and what Path A does on the decode
// thread. DoP turns two source bytes into one frame, and a stage that changed
// the frame rate would be the one thing Path A is defined not to be. Turning a
// container's units into the graph's units is what a codec is for, so this is
// where it goes, and `codec_dsd` reports the DoP form from `get_format` the way
// every other codec reports what it produces.
//
// The packing is the `dop` crate in packer/, which carries the marker phase and
// forbids unsafe. This file is the ABI and the arithmetic that names the rate.
//
// **What is not here: native DSD.** A driver that takes DSD without the PCM
// wrapper -- ASIO's `kAsioSetIoFormat` -- would want these bytes rather than
// the frames, and docs/plan.md §5 records what that is worth: nothing below
// DSD512, because DoP carries DSD256 and a DSD512 stream would need 1411.2 kHz
// PCM. There is no sink here that takes it, so there is no format for it in the
// ABI either; adding one before the sink exists is what §15 says not to do.

#![deny(unsafe_code)]

use mp_abi::{codec, encoding, level, sample, Codec, Error, Format};

/// The most channels either DSD wrapper names.
const MAX_CHANNELS: u32 = 6;

/// A DSD rate is a multiple of DSD64, and DSD1024 is past anything that exists.
const DSD64: u32 = 2_822_400;
const MAX_RATE: u32 = DSD64 * 16;

/// The nine bytes `demux_dsd` assembles: the DSD rate, the channels, the mask.
/// See its `config_bytes` for why this shape and not a standard one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Config {
    dsd_rate: u32,
    channels: u32,
    channel_mask: u32,
}

impl Config {
    fn parse(config: &[u8]) -> Option<Config> {
        if config.len() < 9 {
            return None;
        }
        let dsd_rate = u32::from_le_bytes([config[0], config[1], config[2], config[3]]);
        let channels = u32::from(config[4]);
        let channel_mask = u32::from_le_bytes([config[5], config[6], config[7], config[8]]);
        if channels == 0 || channels > MAX_CHANNELS {
            return None;
        }
        if !(DSD64..=MAX_RATE).contains(&dsd_rate) || !dsd_rate.is_multiple_of(DSD64) {
            return None;
        }
        Some(Config {
            dsd_rate,
            channels,
            channel_mask,
        })
    }

    /// DoP frames a second: the DSD rate over eight for bytes, over two again
    /// because two bytes make a frame. DSD64 is 176400 and DSD256 is 705600.
    fn dop_rate(&self) -> u32 {
        self.dsd_rate / 16
    }
}

pub struct DsdCodec {
    channels: usize,
    format: Format,
    /// Where in the 0x05/0xFA alternation the next frame sits. **Carried across
    /// packets**, because the alternation belongs to the stream: restarting it
    /// every packet puts two identical markers side by side at every packet
    /// boundary, which a DAC reads as a stream it has lost.
    phase: dop::Phase,
}

impl Codec for DsdCodec {
    fn probe(codec_id: u32, config: &[u8]) -> u32 {
        if codec_id == codec::DSD && Config::parse(config).is_some() {
            100
        } else {
            0
        }
    }

    fn open(codec_id: u32, config: &[u8]) -> Result<Self, Error> {
        if codec_id != codec::DSD {
            return Err(Error::Unsupported);
        }
        let Some(cfg) = Config::parse(config) else {
            mp_abi::log(
                level::DEBUG,
                &format!(
                    "the container's DSD configuration is {} bytes and does not parse",
                    config.len()
                ),
            );
            return Err(Error::Format);
        };
        mp_abi::log(
            level::INFO,
            &format!(
                "DSD{} ({} Hz) as DoP: {} Hz / {} ch / 24-bit",
                cfg.dsd_rate / 44100,
                cfg.dsd_rate,
                cfg.dop_rate(),
                cfg.channels
            ),
        );
        Ok(DsdCodec {
            channels: cfg.channels as usize,
            format: Format {
                sample_rate: cfg.dop_rate(),
                channels: cfg.channels,
                channel_mask: cfg.channel_mask,
                sample_type: sample::S24_PACKED,
                encoding: encoding::DOP,
                // **All twenty-four bits are written and none of them is a
                // sample.** Eight are the marker and sixteen are DSD, so there
                // is no narrower container this fits in and no padding to
                // report. Saying 24 is what stops negotiation offering a
                // 16-bit endpoint a stream that would lose half of itself.
                valid_bits: 24,
                reserved: [0; 2],
            },
            phase: dop::Phase::start(),
        })
    }

    fn format(&self) -> Result<Format, Error> {
        Ok(self.format)
    }

    fn decode(&mut self, packet: &[u8], dst: &mut [u8]) -> Result<usize, Error> {
        if packet.is_empty() {
            return Err(Error::Invalid);
        }
        // `demux_dsd` hands out an even number of bytes per channel, so a
        // packet is always a whole number of frames and nothing is ever left
        // over. A packet that is not is a packet from somewhere else, and the
        // odd byte is dropped rather than carried: carrying it would make the
        // next packet's stated frame wrong by half a frame, which is worse.
        let needed = dop::packed_bytes(packet.len(), self.channels);
        if needed == 0 {
            return Ok(0);
        }
        if dst.len() < needed {
            return Err(Error::NoMemory);
        }
        let wrote = dop::pack(packet, self.channels, dst, &mut self.phase);
        if wrote != needed {
            return Err(Error::Internal);
        }
        Ok(wrote)
    }

    fn flush(&mut self, _dst: &mut [u8]) -> Result<usize, Error> {
        // A packet in, a packet out. Nothing is held back, because there is no
        // transform to be halfway through.
        Ok(0)
    }

    fn reset(&mut self) -> Result<(), Error> {
        // **Back to the first marker, and a seek still produces identical
        // bytes.** Those two would fight if a packet were an odd number of
        // frames: a straight decode would arrive at some packet on one phase
        // and a seek to it would start on the other, and every marker byte from
        // there on would differ from the recorded hash.
        //
        // They do not fight, because `demux_dsd` hands out 4096 DSD bytes per
        // channel, which is 2048 DoP frames, which is even -- so the phase at
        // the start of every packet is this one whatever came before it. The
        // alternation across a packet boundary is still correct, and a decode
        // resumed anywhere is byte-identical to a straight one. The phase is
        // carried rather than reset per packet anyway, because that is true of
        // the format and not of one demuxer's block size.
        self.phase = dop::Phase::start();
        Ok(())
    }
}

mp_abi::export_codec!(
    DsdCodec,
    id: "codec_dsd",
    name: "DSD as DoP (written here, in Rust)",
    version: (0, 1, 0),
    priority: 110,
    codecs: [codec::DSD]
);

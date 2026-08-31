<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# What the decoders actually produce

The companion to [devices.md](devices.md): that one records what real hardware
accepts, this one records what real decoders produce. Every row was measured with
`mediaperch-probe decode`, which prints the SHA-256 of the PCM a decoder handed
over, and cross-checked against FFmpeg decoding the same file.

The method matters more than the table. A decoder that quietly converts is
indistinguishable from one that does not, unless you hash the output and compare
it with something that had no reason to make the same mistake.

```
mediaperch-probe decode --file X.flac --decoder native
ffmpeg -i X.flac -f s16le out.raw     # then hash out.raw
```

## The five decoders

| | `decode_flac` | `decode_ogg` | `decode_native` | `decode_mf` | `decode_ffmpeg` |
|---|---|---|---|---|---|
| Behind it | libFLAC | libvorbis, libopus | `dr_wav`, `dr_flac` | Media Foundation | `ffmpeg`, `ffprobe` |
| Covers | FLAC, every depth and rate | Vorbis and Opus in Ogg | WAV, FLAC to 24 bits | MP4/M4A, MP3, WMA, WAV, FLAC | the long tail |
| Priority | 120 | 110 | 100 | 50 | 30 |
| Comes from | `external/flac` | four Xiph submodules | `external/dr_libs` | already on the machine | **found at run time, never shipped** |

Three modules read FLAC on purpose. `decode_flac` outranks the others whenever it
is installed, because for a lossless codec the reference implementation *is* the
specification. `decode_native` stays because an install that wants no submodules
at all should still play music. `decode_mf` scores itself lowest on FLAC and WAV
not because it is worse — the hashes below say it is not — but because it reaches
them through a pipeline that *could* insert a converter, and the others cannot.

`decode_ffmpeg` sits last on purpose. It reads more than anything else here and
knows each format less well than the module that specialises in it, so it takes
what is left: ALAC, WavPack, Monkey's Audio, Matroska, OggFLAC, DSF and DFF. It
also does not ship. It looks for `ffmpeg` and `ffprobe` beside itself and then on
`PATH`, and declines every file when neither is there — which is exactly what an
uninstalled module does.

Going through the executables rather than linking libavcodec is deliberate:
FFmpeg's public structs change layout between major versions, so binding the DLLs
would tie this module to one of them. One build of it works against FFmpeg 4
through 8. §7 of [the plan](plan.md) has the full argument.

### The long tail, measured

| File | Chosen decoder | Reported | Note |
|---|---|---|---|
| Vorbis in Ogg | `decode_ogg` | `44100 Hz / 1 ch / F32` | libvorbis itself |
| Opus in Ogg | `decode_ogg` | `48000 Hz / 1 ch / F32` | libopus through opusfile. Opus decodes at 48 kHz whatever the source rate was; that is the codec, not a resample |
| OggFLAC | `decode_ffmpeg` | `44100 Hz / 1 ch / S16` | `decode_ogg` scores it **0**, not something low: it is an Ogg stream this module cannot read at all, and saying so lets the fallback have it |
| WavPack | `decode_ffmpeg` | `44100 Hz / 2 ch / S32` | hash identical to the 32-bit WAV of the same signal |
| ALAC in M4A | `decode_mf` | `44100 Hz / 2 ch / S24_PACKED` | hash identical to the 24-bit FLAC of the same signal: ALAC round-tripped losslessly. There is no `decode_alac`, and [the plan](plan.md) §7 explains why |
| MP3 | `decode_mf` | `44100 Hz / 2 ch / S16` | Media Foundation outranks FFmpeg here, 100 to 30 |

## Vorbis and Opus: float, and what that costs

`decode_ogg` reports `F32`, because libvorbis and libopus produce float and a
decoder here never converts. So no Vorbis or Opus file takes Path A — and no
implementation of these codecs could, because a lossy codec's output is defined
as a signal within a tolerance, not as a byte pattern. There is nothing for it to
be bit-exact *to*.

What can be measured is whether two independent implementations agree. Against
FFmpeg's own decoders, on the same file, sample for sample:

| File | Frames | Byte lengths agree | max abs difference | SNR |
|---|---|---|---|---|
| Vorbis, 44.1 kHz mono | 264,600 | yes | 5.2 × 10⁻⁸ | 131.8 dB |
| Opus, 48 kHz mono | 288,000 | yes | 1.9 × 10⁻⁸ | 140.4 dB |

That is float rounding. The frame counts and byte lengths matching exactly is the
other half of the result: it means the pre-skip, the header gain and the stream
end are all handled the same way, which is where a decoder integration usually
goes wrong silently.

The same files decoded by the MSVC build and the clang-cl build give **identical
SHA-256s** — all four test files, both codecs. Float arithmetic that came out the
same under two compilers is float arithmetic nobody reordered.

### The channel order, which is the part that can be wrong

Ogg channel layouts are Vorbis's, and Opus mapping family 1 is defined to be the
same. Windows wants WAVE order, and past stereo the two disagree — Vorbis 5.1 is
L,C,R,BL,BR,LFE where WAVE 5.1 is L,R,C,LFE,BL,BR. `decode_ogg` permutes, which
moves samples between slots and never changes a value.

Tested with a 5.1 file carrying a different tone in every channel, checked twice:
against the tones in the source WAV, and against FFmpeg's decode of the same file.

| | Source | Decoded | max abs difference vs FFmpeg | SNR |
|---|---|---|---|---|
| FL | 400 Hz | 400 Hz | 9.7 × 10⁻⁸ | 131.5 dB |
| FR | 800 Hz | 800 Hz | 8.9 × 10⁻⁸ | 131.7 dB |
| FC | 200 Hz | 200 Hz | 7.5 × 10⁻⁸ | 133.7 dB |
| LFE | 1600 Hz | — | 1.5 × 10⁻⁸ | 136.4 dB |
| BL | 3200 Hz | 3200 Hz | 8.2 × 10⁻⁸ | 133.6 dB |
| BR | 6400 Hz | 6400 Hz | 8.9 × 10⁻⁸ | 130.2 dB |

(Vorbis at q8; Opus at 510 kb/s gives the same verdict at 131–140 dB.) The LFE row
has no decoded frequency because both encoders band-limit LFE, so a 1600 Hz tone
put there does not survive — which is the encoder behaving correctly, and is why
that channel is checked against FFmpeg only.

The mask reported for 5.1 is `0x3f`: `FL|FR|FC|LFE|BL|BR`, the WAVE layout, which
is what the sink can pass to `dwChannelMask` unchanged.

### What only libFLAC can do

A FLAC file carries an MD5 of its own unencoded audio, written by the encoder, and
every frame carries a CRC. `decode_flac` turns the check on, so the file itself
can say the decode was wrong:

```
$ mediaperch-probe decode --file corrupt.flac --decoder flac
[warn ] libFLAC: FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH in corrupt.flac
[warn ] libFLAC: FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC in corrupt.flac
```

That is one flipped byte in the middle of the audio, found by the file's own
checksum. No other decoder here can be told it is wrong by its input.

## Bit-exactness

Every decoder that can read a file produces the same bytes as every other. Four
independent implementations, agreeing to the byte, at both extremes:

| File | `flac` | `native` | `mf` | `ffmpeg` |
|---|---|---|---|---|
| 16-bit FLAC | `6ad3ba58` | `6ad3ba58` | `6ad3ba58` | `6ad3ba58` |
| 24-bit FLAC | `7bb1010b` | `7bb1010b` | `7bb1010b` | `7bb1010b` |
| 16-bit WAV | — | `6ad3ba58` | `6ad3ba58` | `6ad3ba58` |
| 32-bit WAV, 768 kHz | — | `29a25188` | `29a25188` | `29a25188` |
| 32-bit FLAC, 1048575 Hz | `3eb96f05` | — | — | `3eb96f05` |

A dash is a refusal, not a mismatch. The detail per format follows.

| File | Reported format | `native` | `mf` | FFmpeg |
|---|---|---|---|---|
| 16-bit WAV, 44100 | `S16` | `6ad3ba58` | `6ad3ba58` | `6ad3ba58` |
| 16-bit FLAC, 44100 | `S16` | `6ad3ba58` | `6ad3ba58` | `6ad3ba58` |
| 24-bit WAV, 44100 | `S24_PACKED` | `7bb1010b` | `7bb1010b` | `7bb1010b` |
| 24-bit FLAC, 44100 | `S24_PACKED` | `7bb1010b` | `7bb1010b` | `7bb1010b` |
| **32-bit WAV, 768000** | `S32` | `29a25188` | `29a25188` | `29a25188` |
| **32-bit WAV, 1048575** | `S32` | `3eb96f05` | `3eb96f05` | `3eb96f05` |
| 24-bit FLAC, 768000 | `S24_PACKED` | `4b2e98d0` | refused | `4b2e98d0` |
| 24-bit FLAC, 1048575 | `S24_PACKED` | `14693cb7` | refused | `14693cb7` |
| **32-bit FLAC, 44100** | `S32` | `faf4b06d` (`flac`) | refused | `faf4b06d` |
| **32-bit FLAC, 768000** | `S32` | `29a25188` (`flac`) | refused | `29a25188` |
| **32-bit FLAC, 1048575** | `S32` | `3eb96f05` (`flac`) | refused | `3eb96f05` |

The last three rows are `decode_flac`; `decode_native` refuses them (see below).
Note that the 32-bit FLAC hashes equal the 32-bit WAV hashes two rows up: the FLAC
round trip is lossless all the way through, at the spec's ceiling rate.

**Media Foundation is bit-exact for WAV and FLAC.** That was not safe to assume:
a source reader will insert a converter to produce whatever media type it is asked
for, and the conversion is invisible. `decode_mf` therefore asks for the *native*
depth, reads back what the reader agreed to, and reports that rather than what it
asked for — see §14 of [the plan](plan.md).

One difference, and it is metadata rather than samples: for FLAC, Media Foundation
reports a channel mask (`0x3` for stereo) where `dr_flac` reports none, because
FLAC has no channel-mask field and the two disagree about supplying the
conventional one. It changes which candidate is offered to a device first and
nothing else.

## 32-bit FLAC: what `dr_flac` cannot do, and what closed the gap

FLAC has allowed 32-bit samples since version 1.4. The reference encoder produces
them; `flac 1.5.0` encoded all three test files, and its own decoder round-tripped
them back to hashes identical to the 32-bit WAVs they came from. The files are
good.

**`dr_flac` cannot decode them, and does not say so.** Its frame-header table

```c
const drflac_uint8 bitsPerSampleTable[8] = {0, 8, 12, -1, 16, 20, 24, -1};
```

still marks index 7 as reserved, which is the code FLAC 1.4 assigned to 32 bits,
and `DRFLAC_ASSERT(pFlac->bitsPerSample <= 24)` appears throughout its decode
paths. The result is not an error: it opens the file, reports 32 bits from
STREAMINFO, and decodes **zero frames**. Media Foundation refuses these files
outright, which is at least honest.

For a player whose entire claim is bit-exactness, a decoder that produces silence
of length zero is the worst failure mode available: `read` returns 0, the graph
calls that the end of the stream, and the track is skipped without a word. So
`mp::Decoder::open` now decodes one frame and rewinds before declaring success,
and a decoder that produces nothing is refused with a sentence rather than
accepted with an empty file:

```
f32_44k.flac    cannot decode: the decoder opened the file and produced no audio at all
```

That check costs one frame at open and guards every decoder, including ones not
written yet.

**The gap is closed by `decode_flac`**, which uses libFLAC itself: all three files
decode to hashes identical to the reference decoder's, including at 1,048,575 Hz.
FFmpeg also decodes them correctly, so `decode_ffmpeg` would have worked too —
libFLAC is the smaller answer, builds with CMake, and brings the MD5 check with
it.

## Limits found by testing the edges

- **`dr_wav` refuses anything above 384 kHz** — `DRWAV_MAX_SAMPLE_RATE`, its own
  sanity ceiling against garbage headers, not a WAV limit: the field is 32 bits
  wide. It turned a perfectly good 768 kHz file into "unsupported by this module",
  which reads like our decision rather than a vendored constant. Raised to
  6,144,000 Hz, which is past every rate that exists while still bounding what a
  malformed header can claim.
- **Media Foundation will not decode FLAC above about 655 kHz.** It handles 32-bit
  WAV at 1,048,575 Hz without complaint, so this is its FLAC decoder rather than
  the framework. The two decoders cover each other exactly here, which is the
  clearest argument the module architecture has produced so far: neither one alone
  reads everything in this table.
- **FFmpeg's FLAC *encoder* writes 24 bits even when given `-sample_fmt s32`**, so
  a genuine 32-bit FLAC needs the reference encoder. `dr_flac` *looked* ready for
  one — it rejects only `subframeBitsPerSample > 32` — which is exactly why
  looking ready is not evidence. See the section above.
- **Above 655350 Hz a FLAC leaves the streamable subset**, because the frame
  header can only carry a rate in Hz (16 bits), in kHz (8 bits) or in tens of Hz
  (16 bits); anything larger has to be read from STREAMINFO. `flac` refuses with
  `FLAC__STREAM_ENCODER_INIT_STATUS_NOT_STREAMABLE` until given `--lax`. The files
  are legal FLAC; they are just not in the subset every decoder promises.
- **FFmpeg's FLAC encoder also picks an invalid block size above ~600 kHz**,
  choosing 65536 when the format's maximum is 65535, and refuses its own choice.
  `-frame_size 4096` works around it.

## Fuzzing

`dr_wav` and `dr_flac` have libFuzzer targets in `fuzz/`, built with ASan and run
for thirty seconds each in CI. A longer local campaign — eight minutes across the
two, at about 150,000 executions a second — found **no crashes**, and grew the
coverage corpus to 474 WAV and 2,113 FLAC inputs.

That is a short campaign and it proves correspondingly little. What it does prove
is that the machinery works, which is the part that rots.

## Still untested

- 8-bit WAV, which is unsigned and would need a conversion to reach any of our
  sample types; `decode_native` refuses it deliberately rather than converting.
- Anything above two channels, at any depth.
- DSD in any form: DoP is designed for and not implemented.

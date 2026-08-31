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

## The six decoders

| | `decode_flac` | `decode_alac` | `decode_ogg` | `decode_native` | `decode_mf` | `decode_ffmpeg` |
|---|---|---|---|---|---|---|
| Behind it | libFLAC | **nothing** | libvorbis, libopus | `dr_wav`, `dr_flac` | Media Foundation | `ffmpeg`, `ffprobe` |
| Covers | FLAC, every depth and rate | ALAC in M4A | Vorbis and Opus in Ogg | WAV, FLAC to 24 bits | MP4/M4A, MP3, WMA, WAV, FLAC | the long tail |
| Priority | 120 | 115 | 110 | 100 | 50 | 30 |
| Probe score for FLAC | 100 | — | — | **60** | 40 | 30 |
| Comes from | `external/flac` | **this tree** | four Xiph submodules | `external/dr_libs` | already on the machine | **found at run time, never shipped** |

Score is the registry's primary key and priority only breaks ties, so the score
is where a statement about *one format* belongs. `decode_native` scores FLAC at
60, below every other FLAC reader here, because `dr_flac` is a reimplementation
that cannot read 32-bit FLAC at all — and above `decode_mf` and `decode_ffmpeg`,
so an install with no submodules and no FFmpeg still plays FLAC. Its WAV score
stays at 100: there is no reference implementation to defer to, and `dr_wav` is
measured bit-exact to 32 bits and 768 kHz.

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
| ALAC in M4A | `decode_alac` | up to `384000 Hz / 8 ch / S32` | ours, and the only decoder here with no dependency at all. See below |
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

### Reference or FFmpeg: which is more faithful?

Neither, measurably. Six seconds of pink noise plus a tone, encoded and then
decoded both ways and compared against the file that went *into* the encoder:

| | Opus, 128 kb/s | Vorbis, q5 |
|---|---|---|
| SNR vs source, libopus/libvorbis | 9.78 dB | 12.61 dB |
| SNR vs source, FFmpeg's own decoder | 9.78 dB | 12.61 dB |
| difference between the two decoders | 134.8 dB down | 132.6 dB down |

The two decoders are the same distance from the source to four decimal places,
and the codec's own loss sits **120–125 dB above** the gap between them. Pink
noise is a deliberately harsh case — a perceptual codec throws away a great deal
of it and sounds fine doing so — which is exactly why it makes the point: the
choice of decoder is not where the fidelity went.

So the question is settled by the specification rather than by measurement, and
the two codecs settle it differently:

- **Opus** is defined *by* its reference implementation. RFC 6716 ships the
  conformance procedure as a shell script, which is in the submodule at
  `external/opus/tests/run_vectors.sh`, marked "extracted from RFC6716" — and
  that procedure is not a bit comparison. It runs `opus_compare`, which measures
  a psychoacoustically weighted error across 21 bands and reports a quality
  percentage; the script accepts either of two reference outputs, because float
  implementations legitimately differ. libopus is the yardstick, and FFmpeg's
  decoder is a thing measured against it.
- **Vorbis** has a prose specification, in this tree at
  `external/vorbis/doc/`, with libvorbis as its reference implementation.

Preferring the reference is therefore a decision about *provenance* rather than
about output quality: a bug in libvorbis is a bug in the definition and gets
fixed there, while a divergence in a reimplementation has to be found first.
`decode_ffmpeg` remains one command away for anyone who disagrees, which is the
point of the module boundary.

## ALAC, decoded here

`decode_alac` is the only decoder in this tree with no dependency of any kind:
no submodule, no runtime library, no OS codec. Both the ALAC bitstream and the
slice of MP4 needed to find its packets are in `modules/decode_alac`. [The
plan](plan.md) §7 has the argument; the short version is that Apple's reference
implementation is simultaneously the specification and unmaintained since 2011,
and the second half of that is what ALHACK was.

### Bit-exact to the ceiling of the format

ALAC tops out at 32 bits, 384 kHz, 7.1. Every row here was encoded from the WAV
in the first column with `refalac` — Apple's own reference *encoder*, so the
files are not FFmpeg's idea of ALAC — and decoded back:

| Channels | Rate | Depth | `decode_alac` | `decode_mf` | `decode_ffmpeg` |
|---|---|---|---|---|---|
| 1 | 44100 | 16 | **=** | = | = |
| 2 | 44100 | 16 | **=** | = | = |
| 2 | 44100 | 20 | **=** | = | widened to S32 |
| 2 | 44100 | 24 | **=** | = | = |
| 2 | 44100 | 32 | **=** | = | = |
| 2 | 96000 | 24 | **=** | = | = |
| 2 | 192000 | 32 | **=** | = | = |
| 2 | 384000 | 32 | **=** | = | = |
| 6 | 48000 | 24 | **=** | refused | = |
| 6 | 48000 | 32 | **=** | refused | = |
| 8 | 48000 | 16 | **=** | refused | = |
| 8 | 96000 | 24 | **=** | refused | = |
| 8 | 384000 | 32 | **=** | refused | = |

**=** means the SHA-256 of the decode equals the SHA-256 of the source WAV: not
close, identical. MSVC and clang-cl produce the same hashes for all of them.

Two rows are worth a second look. The **20-bit** one is a depth nothing else
here reports honestly: `decode_alac` says `S24_PACKED (20 valid)` and
left-justifies, while FFmpeg widens to `S32` — not a mismatch, a different
container, and the reason those two hashes differ. The **8-channel** rows are
the ones `decode_mf` declines, for the reason below.

Seeking is checked the only way that makes it falsifiable: the hash of a decode
that seeks to frame N must equal the hash of the last (length − N) frames of a
straight decode. It does, at packet boundaries and inside them, and matches
FFmpeg seeking to the same frames. `--seek` exists so that this can be asked
from the command line — before it, no decoder's seek had ever been tested.

### What Media Foundation does with multichannel ALAC

`decode_mf` declines ALAC past stereo, and this is why. A 7.1 file was decoded
and compared channel by channel against the WAV it came from:

| | Slots matching the source |
|---|---|
| `decode_alac` | **8 of 8** |
| `decode_ffmpeg` | **8 of 8** |
| `decode_mf` | **0 of 8** |

Media Foundation did not lose a single sample — every channel came back
somewhere, exactly. It came back in the *wrong slot*. MF returns ALAC in Apple's
channel order, `C, Lc, Rc, L, R, Ls, Rs, LFE`, while labelling it with a WAVE
channel mask that says `FL, FR, FC, LFE, BL, BR, FLC, FRC`. The mask and the
samples disagree and nothing in the output looks wrong.

The same permutation appears whether the file was written by FFmpeg or by
`refalac`, so it is Media Foundation's, not a muxing artefact. Stereo and mono
are untouched — a two-channel file cannot have a layout bug — and 8-channel WAV
and FLAC give one hash across every decoder including MF, so it is ALAC-specific.
MF refuses 8-channel AAC outright, which is the honest failure and the one this
makes ALAC match.

`decode_alac` applies that permutation itself, from ALAC's own channel table, and
a unit test asserts that every layout from 1 to 8 channels is a permutation
rather than a rearrangement that drops or duplicates one — which is precisely the
bug that is inaudible on most material and puts the centre channel in the wrong
speaker.

### What was written instead of linked

Writing the decoder rather than vendoring one found five things Apple's does not
check, each reachable from a file:

| In the reference | What a crafted file gets |
|---|---|
| `1 << (denshift - 1)`, computed on entry | undefined shift; `denshift` is four bits from the stream |
| `x >> (32 - k)`, `(1 << k) - 1` | undefined shift; `k` is derived from decoded values |
| warm-up writes `numactive` samples | a write past the frame buffer when a partial frame is shorter than the coefficient count |
| shift buffer ORed into the output | the previous frame's contents, when `mixres` is set and `bytesShifted` is not |
| `read32bit(in + (bitPos >> 3))` | a four-byte read past the packet, on the last sample of every frame |

The last one is not even a crafted-file problem: it happens on every valid file
and is only harmless because the allocation usually has slack. Here the bit
reader zero-pads a peek past the end and checks *consumption* instead, which is
where the answer means something.

`fuzz/alac_fuzzer.cpp` drives the container parser and the codec from the same
input. That harness is the point: a new decoder is only better than an
unmaintained one if somebody is actually looking.

## When the best decoder says no

Probing reads four kilobytes. Opening reads the whole header and, because
`mp::Decoder` requires one frame of real audio before it calls an open a
success, a frame of audio too. So a decoder can score highest and still refuse,
and there are now two that do it for good reasons — `decode_mf` on multichannel
ALAC, `decode_native` on 32-bit FLAC.

The registry therefore ranks every decoder that claims the file and the host
walks the list, so a refusal costs the next candidate rather than the file:

| File | Ranked | Opens | Used |
|---|---|---|---|
| 32-bit FLAC | flac, native, mf, ffmpeg | flac | `decode_flac` |
| 7.1 ALAC | alac, mf, ffmpeg | alac | `decode_alac` |
| AAC in M4A | alac, mf, ffmpeg | mf | `decode_mf` |
| 8-channel AAC | alac, mf, ffmpeg | ffmpeg | `decode_ffmpeg` |
| Vorbis | ogg, ffmpeg | ogg | `decode_ogg` |

The AAC row shows the cost, and it is deliberate. `decode_alac` scores 100 on
every MP4, not only the ones with `alac` visible in the first four kilobytes —
because whether it is visible depends on where the muxer put `moov`, and FFmpeg
puts it last while `refalac` puts it first. Scoring on that would make the chosen
decoder depend on which program wrote the file. Looking first costs two seeks and
a few hundred kilobytes; the alternative costs the ability to reason about it.

`--decoder` does not get a fallback. Being told "use that one" and answering
with a different one is not an answer; a forced decoder that refuses reports the
refusal.

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

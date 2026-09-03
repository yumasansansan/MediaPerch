<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# What the decoders actually produce

The companion to [devices.md](devices.md): that one records what real hardware
accepts, this one records what real decoders produce. Every row was measured with
`mediaperch-probe decode`, which prints the SHA-256 of the PCM a decoder handed
over, and cross-checked against FFmpeg decoding the same file.

The method matters more than the table. A decoder that quietly converts is
indistinguishable from one that does not, unless you hash the output and compare
it with something that had no reason to make the same mistake.

**The summary is in the [README](../README.md), and it is generated.**
`ctest -R format_matrix` builds one file per format, shows it to every decoder in
the tree, and fails if what it measures is not what the README says. Its lossless
corpus comes from the reference encoders -- `flac` and `refalac` -- rather than
from FFmpeg, for the reason recorded under *Limits found by testing the edges*
below. This file is the detail behind that table: how each number was reached and
what it cost to find out.

Generating it immediately earned its keep. Two rows below had quietly stopped
being true -- MP3 went to `decode_mf` until `decode_mp3` was written, and the
*Still untested* list still said multichannel was untested three sections after a
7.1 measurement. Both are corrected here, and neither would have been noticed by
reading.

```
mediaperch-probe decode --file X.flac --decoder flac
ffmpeg -i X.flac -f s16le out.raw     # then hash out.raw
```

## The eight decoders that used to be here

Every measurement below this line was first taken through a module that was a
container reader and a codec in one object -- `decode_flac`, `decode_alac`,
`decode_ogg`, `decode_aac`, `decode_mp3`, `decode_native`, `decode_ffmpeg` and
`decode_mf`. All eight are gone, and each was replaced by a demuxer and a codec
that decode to the same bytes, checked file by file in the table after next.

The two that could not be split are still here under different names:
`decode_ffmpeg` and `decode_mf` became `demux_ffmpeg` and `demux_mf`, with every
stream flagged `MP_STREAM_SELF_DECODES`. That is a declaration rather than a
disguise -- neither is a container reader that happens to decode; each is a
pipeline with a file at one end and PCM at the other.

## The eight demuxers and seven codecs that replaced them

[ABI v2](plan.md#abi-v2-the-container-decides) split a decoder into a container
reader and a packet decoder, and the migration is finished: **every format this
tree reads resolves container-first**. A file is identified, opened, and asked
what is in it; each stream names its codec and the codec is looked up. Nothing
is tried.

| Container | Behind it | Codec it names | Behind that |
|---|---|---|---|
| `demux_wav` | dr_wav | `codec_pcm` | **nothing at all** -- it is a memcpy |
| `demux_flac` | libFLAC | `codec_flac` | libFLAC |
| `demux_mpeg` | **nothing** | `codec_mp3` | dr_mp3 |
| `demux_adts` | **nothing** | `codec_aac` | **nothing** |
| `demux_mp4` | Bento4 | `codec_alac`, `codec_aac` | **nothing** |
| `demux_ogg` | libogg | `codec_opus`, `codec_vorbis`, `codec_flac` | libopus, libvorbis, libFLAC |
| `demux_mkv` | libmatroska | seven of them, see below | |
| `demux_ffmpeg` | `ffmpeg`, `ffprobe` | *itself* -- `SELF_DECODES` | |
| `demux_mf` | Media Foundation | *itself* -- `SELF_DECODES` | |

The `codec_flac` row is the one worth reading twice. It is the codec half of the
native FLAC reader, what `demux_ogg` hands an OggFLAC to, *and* what `demux_mkv`
hands a FLAC-in-Matroska to -- and none of the three containers knows the others
exist. That is the whole argument for the split in one line of a table.

### MP4, and the parser that is no longer here

`demux_mp4` read MP4 with a parser written in this tree until it didn't. What it
read was twelve boxes of ISO 14496-12 -- `moov`, `trak`, `mdia`, `minf`, `stbl`
as containers; `stsd` for the sample entry; `stsz`, `stco`/`co64`, `stsc` and
`stts` for where the packets are and how long they run; `mdhd` and `mvhd` for the
two timescales; `elst` for the gapless edit -- and two sample entries, `alac` and
`mp4a`. That is the shape a music file has and nothing else, and it was about
five hundred lines.

**What it refused, measured with files built for the purpose, is why it is
gone:**

| File | The old parser | On Bento4 |
|---|---|---|
| M4A, ALAC | 88200 frames, byte-identical to the source WAV | **unchanged** |
| M4A written `-movflags +faststart` | reads it | **unchanged** |
| MP4 written `-movflags +frag_keyframe+empty_moov` | `unsupported by this module`, FFmpeg read it | **90112 frames, the same as FFmpeg** |
| MP4 written `-movflags +dash` | `unsupported by this module`, FFmpeg read it | **90112 frames, the same as FFmpeg** |
| QuickTime `.mov`, ALAC | `unsupported by this module`, FFmpeg read it | **byte-identical to the source WAV** |

Fragmented MP4 was the gap that mattered. `moof`/`traf`/`trun` is how every DASH,
CMAF and HLS-fMP4 stream is written, and how `ffmpeg` writes an MP4 when told to
make one that can be produced without seeking backwards -- there is no `stbl` in
such a file at all, so nothing the old parser knew how to read was there.

**Two things had to be found by measurement rather than by reading Bento4's
headers**, and both are in the module with the numbers beside them:

- **A `.mov` keeps the ALAC configuration somewhere else.** An MP4 has the
  `alac` box as a direct child of the sample entry; QuickTime wraps it in
  `wave`, its own decompression-parameter container, beside `frma` and `chan`.
  Looking only in the first place made the module open every `.mov`, find the
  track, and then say "nothing here decodes that codec".
- **`AP4_LinearReader` starts looking for fragments wherever the stream is.**
  Its constructor takes the current position as the first fragment's, and
  parsing the file has left that at the end -- so a fragmented MP4 opened
  cleanly, reported its track, and produced *zero* samples against ffmpeg's
  90112, because every `moof` was already behind the cursor. It is rewound now.

**Seeking got better rather than merely surviving.** The old module divided by a
frames-per-packet taken from the first `stts` entry; this one asks `stts` which
sample contains the frame. Measured against a WAV of the same audio, ALAC in
M4A and ALAC in `.mov` both come back byte-identical at frames 0, 1, 1000,
44100, 44101 and 88199 -- and a fragmented MP4, which has no sample table at all
and is seeked through the reader, lands on the exact frame asked for at 0, 1024
and 44100.

**What is still not read.** `cenc` encryption; sample entries other than `alac`
and `mp4a`, which leaves FLAC-in-MP4 (`dfLa`) and Opus-in-MP4 (`dOps`) unnamed
even though this tree decodes both -- Bento4 has no class for either, so they
would need the same hand extraction `alac` gets; and edit lists past the first
entry. Multi-track files *are* read now: every track is reported, the way
`demux_mkv` reports them, rather than the first audio one being the only stream
the module admitted to.

### Bento4 or GPAC, and why Bento4

Checked against both checkouts rather than from memory:

| | Bento4 | GPAC |
|---|---|---|
| Licence | GPL-2.0-**or-later**, dual-licensed (commercial available) | LGPL-2.1-or-later |
| Compatible with this tree | yes -- "or any later version" is what makes it so | yes |
| Lines of source | ~80,600 | ~964,800 |
| Scope | ISOBMFF, and only that | a whole multimedia framework: filter graph, streaming server, DASH packager, 25 more container formats |
| Build system | **CMake** | `./configure` + Makefile, or hand-kept MSVC projects |
| Reader API | `AP4_Track` / `AP4_SampleTable`, C++ | `gf_isom_open` / `gf_isom_get_sample`, C |

The licence question resolves the other way from what it looks like at a glance.
Bento4 being "GPL" reads as a problem for a GPL-3.0-or-later tree, and it would
be if it were GPLv2-**only**. Its file headers say *"either version 2, or (at your
option) any later version"*, so it may be taken as GPLv3 and linked here.

What decides it is the other two rows. **GPAC has no CMake build**, and every
dependency here is a submodule that `add_subdirectory` picks up and builds with
the same compiler and flags as everything else -- `docs/building.md` is most of a
page on why that is not negotiable. And GPAC is a framework twelve times the
size, of which `src/isomedia` is the part wanted; taking it means taking its
configure script, its platform layer and its build assumptions to reach one
directory. Bento4 is an ISOBMFF library with a CMakeLists.txt, which is the shape
of every submodule already here.

**It is pinned past its last release tag on purpose.** Bento4 tags rarely: 119
commits separate `v1.6.0-641` from the pinned one, and among them are a heap
overflow in `AP4_BitReader::ReadCache`, a heap overflow in the AC-3 parser, and a
leak in `AP4_LinearReader::Tracker` -- which is the class this module uses on
every fragmented file.

### What the change cost, and what the fuzzer found in ten minutes

Deleting `modules/shared/mp4` deleted a fuzz target with it, and swapping five
hundred fuzzed lines for eighty thousand unfuzzed ones is not a trade this tree
makes. `fuzz/mp4_fuzzer.cpp` walks the path `demux_mp4` walks -- open, enumerate
tracks, read every sample through `AP4_LinearReader` -- over a seed corpus of
plain ALAC, AAC, a fragmented MP4 and a `.mov`. Bento4 is compiled into it with
the sanitizer *and coverage* on rather than linked from the main build: a library
linked in uninstrumented is a library the fuzzer runs through and reports nothing
about, and the first campaign here proved it by reporting "36 inline 8-bit
counters" -- the harness, and nothing else. With coverage it reports 15,865.

**It found two denials of service in Bento4, both the same defect.** A box states
an entry count; the parser loops that many times; nothing checks the count
against the bytes the box actually has.

| Box | Declared entries | Box size | Measured |
|---|---|---|---|
| `sgpd` | 67,108,865 | 26 bytes | **2 GB and no return** -- one allocation per entry, from a 1143-byte file |
| `dref` | 956,301,312 | 28 bytes | **84 seconds** -- the inner loop drains the stream on the first pass, so the rest spin on nothing |

Both files are kept, as `fuzz/corpus/mp4/found_sgpd_unbounded_entries.m4a` and
`found_dref_unbounded_entries.mp4`. `mediaperch-probe claims` on either of them
did not come back. They now return in about 220 ms.

**Two defences, because one was not enough.**

The blunt one is a *budget on the stream*: `FileStream` counts reads and seeks,
each entry point arms a limit, and past it every operation reports end-of-stream
-- which every parser in Bento4 already handles, because a truncated file does
the same. It bounds any parse loop that touches the file, including ones nobody
has found yet. A million operations for an `open` is far above any real file:
parsing is proportional to a file's *boxes*, not its samples.

The sharp one is *not parsing three boxes at all*. The budget does not catch
`dref`, because its spin never reads a byte -- `bytes_available < 8` returns
before any I/O -- and that is exactly why both are here. `sgpd`, `sbgp` and
`dref` are declined in the atom factory: sample groups describe roll distances
for editors and packagers, and `dref` says which file the media lives in, which
this tree answers by only reading self-contained ones. Nothing in Bento4 reads a
parsed `dref` either -- the only other mention of the class is `Ap4TrakAtom.cpp`
constructing one when *writing* a track.

**The durable fix is upstream and it is small.** In `Ap4SgpdAtom.cpp`, subtract
each entry from `bytes_available` as it is read, and stop after the first entry
when the version is 0, since a version-0 entry consumes the rest of the box by
definition. In `Ap4DrefAtom.cpp`, leave the outer loop when the inner one adds
nothing. Until that lands, the two defences above stand, and the fuzzer keeps
running without the budget so that the next one of these is reported rather than
quietly absorbed.

### Where the reference implementation is used, and where it is not

**Three of these read a container with a library written by the people who
define the format**, and one of those three did not always. `demux_flac` was
hand-written first, on the reasoning that libFLAC does not expose frame
boundaries -- which is wrong: `FLAC__stream_decoder_skip_single_frame` advances
one frame without reconstructing it and `FLAC__stream_decoder_get_decode_position`
says where that left the stream, and Xiph's own header documents the pair for
*"separating a FLAC stream into frames for editing or storing in a container"*.
So the tree had a scan of its own standing in front of the reference
implementation of the same format. It does not now, and the six FLAC files below
decode to the same bytes as before the change, including 32-bit and 768 kHz.

The parser that was deleted had cost something. `flacframe_fuzzer` found an
out-of-bounds read in it within ninety seconds -- a frame header can parse in
fewer bytes than the shortest frame it implies, and the CRC scan began past the
end of its buffer. libFLAC's decoder is fuzzed continuously on OSS-Fuzz, which no
local campaign matches.

Where a container is still read by code here, it is because there is no library
that reads only the container: `demux_mp4`'s parser is a slice of ISO 14496-12
(see below), `demux_mpeg` and `demux_adts` are frame headers, and dr_wav is a
header rather than a build.

### Matroska, which is what a container reader was for

`demux_mkv` reads Matroska and WebM on libebml and libmatroska. It is the
container that makes the v2 split pay for itself twice over.

**Seven codecs at once, and no decoding written.** Matroska carries FLAC, Vorbis,
Opus, AAC, MP3, ALAC and PCM, and every one already had a codec module here. The
module maps `A_FLAC`, `A_VORBIS`, `A_OPUS`, `A_AAC`, `A_MPEG/L1..L3`, `A_ALAC`
and `A_PCM/INT/LIT` onto MpCodec, converts each codec's `CodecPrivate` into the
blob the ABI defines, and hands over packets. Under v1 an `.mka` went to FFmpeg
entire, because "which decoder reads this file" has no answer for a container
that could hold any of them.

Measured against FFmpeg on the same files:

Measured against FFmpeg on two seconds of the same audio, every file made from
one 88200-frame WAV:

| File | Through | Frames | FFmpeg | Against FFmpeg |
|---|---|---|---|---|
| FLAC in Matroska | `demux_mkv` + `codec_flac` | 88200 | 88200 | **byte-identical**, and identical to the source WAV |
| ALAC in Matroska | `demux_mkv` + `codec_alac` | 88200 | 88200 | **byte-identical**, and identical to the source WAV |
| PCM in Matroska | `demux_mkv` + `codec_pcm` | 88200 | 88200 | **byte-identical**, and identical to the source WAV |
| AAC in Matroska | `demux_mkv` + `codec_aac` | 89088 | 89088 | same length; two AAC decoders, so not the same bytes |
| Vorbis in Matroska | `demux_mkv` + `codec_vorbis` | 88200 | 88200 | same length, and the source's |
| MP3 in Matroska | `demux_mkv` + `codec_mp3` | 88200 | 88200 | same length, and the source's |
| Opus in WebM | `demux_mkv` + `codec_opus` | 96000 | 96000 | same length -- 2.000 s at Opus's 48 kHz |

**The three lossless rows being byte-identical is the result that matters**: a
container reader that changed a sample would be a container reader that was
wrong, and Matroska is by some distance the most complicated container this tree
reads. **The four lossy rows landing on the exact frame is the second result**,
and it took an ABI field to get there.

### Where a lossy track ends, and why `play_frames` could not say it

Matroska states the encoder's head padding per track, as `CodecDelay`, and this
module converts it to `MpStreamInfo::skip_frames` -- the same fact `elst` carries
in MP4 and `pre_skip` carries in an Opus header, spelled a third way, and the
reason that field is the container's rather than the codec's.

The tail took longer, and the reason is worth writing down. Matroska states it in
`DiscardPadding`, which lives in a `BlockGroup` beside the last block -- which is
why a muxer writes the final block of a track as a group rather than a
SimpleBlock. But `DiscardPadding` is **how many frames at the end are padding**,
and `play_frames` is **how long the audio is**, and turning the first into the
second needs the decoded length, which Matroska cannot state: every timestamp in
the file is scaled to the millisecond. Measured on the Opus file, whose last
block is at 2001 ms with a 7 ms `BlockDuration` and 13.5 ms of padding:

| From the file | Frames | Against FFmpeg's 96000 |
|---|---|---|
| `Duration`, the segment's own length | 96648 | 648 too many -- the padding, played |
| block timestamp + `BlockDuration` | 96384 | 384 too many |
| the same, minus `DiscardPadding` | 95736 | 264 **too few** -- real audio cut |
| decoded length - `CodecDelay` - `DiscardPadding` | 96000 | **exact** |

The last row is exact because it never divides: 13.5 ms of padding is 648 frames
of 48 kHz however the timestamps were scaled, and 6.5 ms of delay is 312. The
three rows above it all pass through a millisecond-rounded timestamp. So the
padding is stated as itself, in `MpStreamInfo::trim_frames`, and the host holds
back that many frames and drops them when the packets run out. `play_frames`
stays 0 for Matroska, which is the honest answer: the file does not say.

Two smaller things came out of the same measurement.

**Nanoseconds convert by rounding, not truncation.** Every one of these numbers
was a frame count before the muxer wrote it, and a whole number of frames rarely
lands on a whole nanosecond -- an MP3's 1105-frame delay is 25056689.34 ns, and
ffmpeg writes 25056689, which truncates back to 1104. One frame, on every file,
until `to_frames` rounded.

**Vorbis states a `CodecDelay` that libvorbis has already applied**, and this
module ignores it for that codec alone. Vorbis has no delay of its own -- Ogg
trims its head with a granule position and nothing else -- and libvorbis returns
nothing at all for the first packet, because a window with nothing to lap against
produces no samples. ffmpeg writes `CodecDelay: 2902494` anyway, describing its
own decoder, which does emit them. Measured: libvorbis returns 88320 frames for
this file, the padding is 120, and 88320 - 120 is the source's 88200 exactly;
subtracting the stated 128 as well cuts 128 frames of real audio off the front.
The same Vorbis read as Ogg comes out at 88200 with no delay applied, which is
the other half of the measurement.

### libmatroska or libwebm, and why this one

Google's [libwebm](https://github.com/webmproject/libwebm) reads the same bytes,
and the choice between them is closer than the names suggest. Checked against
both checkouts rather than from memory:

| | libmatroska (+ libebml) | libwebm |
|---|---|---|
| Licence | LGPL-2.1-or-later | BSD-3-Clause |
| Lines of source | ~12,600 | ~35,800, of which `mkvparser` is a fraction |
| Scope | the whole Matroska schema: **258 element classes** | the WebM subset: 22 parser classes |
| Also writes files | yes | yes (`mkvmuxer`) |
| What this module needs | all present | all present: `GetCodecDelay`, `GetDiscardPadding`, `GetDuration`, lacing, `CodecPrivate`, Cues |
| Fuzzed upstream | no in-tree target | two: `mkvparser_fuzzer`, `webm_fuzzer` |

**libwebm would have worked.** Its `mkvparser` does not gate on `DocType` -- it
requires one to be present and the versions to be sane, and nothing more -- so it
reads a plain `.mka` as readily as a `.webm`, and every element this module reads
has an accessor on it. On two counts it is the better-behaved dependency: BSD
rather than LGPL, and two fuzz targets maintained upstream against libmatroska's
none.

It was not chosen for one reason, and it is the reason recorded at the top of
this document for FLAC: **libmatroska is the format's own implementation.** It is
written by the people who define Matroska, it is what the specification is
checked against, and its 258 element classes are the schema itself rather than a
profile of it -- Chapters, Attachments, Tags, `ContentEncoding`, the lot. libwebm
is Google's reader for the subset Chrome plays, and its 22 classes are exactly
that subset. This tree already reads Matroska with FLAC, ALAC, MP3 and AAC in it,
none of which appears in a WebM file, and §9's video path will want more of the
schema rather than less.

The trade is honest and worth stating rather than hiding: this tree took the
complete schema and gave up a permissive licence and upstream fuzzing for it.
LGPL-2.1-or-later is compatible here -- it may be taken as LGPLv3, which GPLv3
links -- and `docs/plan.md` records that check beside every other submodule's.

**A file is genuinely several streams, and this is where that stops being
theory.** Given a Matroska with H.264 video, a FLAC track and an Opus track,
`mediaperch-probe claims` prints:

```
demux_mkv        100  Matroska and WebM (libmatroska, the reference container)
  stream 0  video    unknown  -> nothing here decodes it
  stream 1  audio    FLAC     -> codec_flac
  stream 2  audio    Opus     -> codec_opus
demux_ffmpeg     100  FFmpeg (found at run time, not shipped)
  stream 0  audio    internal -> this module, itself
  stream 1  audio    internal -> this module, itself
```

Three streams against two, and the video track named rather than hidden. §9's
video path will ask for it. `select` picks between them and the host takes the
default audio track.

**Seeking is by Cues**, Matroska's own index of timestamp to cluster position,
which lands on a cluster boundary at or before the target; `MP_PACKET_TIMED` on
the first block is what lets the host discard the rest. Measured on the three
lossless Matroska files against a WAV of the same audio: frames 0, 1000, 44100
and 44101 all produce identical bytes.

**Nothing about the audio changed, and that is the measurement that matters.** A
split that cost a sample would not be worth making, so every file that crossed
was decoded both ways and hashed:

| File | Through | SHA-256 of the PCM | Against v1 |
|---|---|---|---|
| WAV, 16-bit 44.1 kHz stereo | `demux_wav` + `codec_pcm` | `b38bebc6…1af5b059` | identical |
| WAV, 24-bit 96 kHz stereo | `demux_wav` + `codec_pcm` | `f434a906…ef3abf1a` | identical |
| WAV, 32-bit float 48 kHz | `demux_wav` + `codec_pcm` | `b2290d59…f19597ce` | identical |
| WAV, 16-bit 5.1 at 48 kHz | `demux_wav` + `codec_pcm` | `5c449afc…621c8a91` | identical |
| AIFF and Sony Wave64, 16-bit 44.1 | `demux_wav` + `codec_pcm` | `b38bebc6…1af5b059` | identical |
| FLAC, 16-bit 44.1 kHz stereo | `demux_flac` + `codec_flac` | `b38bebc6…1af5b059` | identical |
| FLAC, 24-bit 96 kHz stereo | `demux_flac` + `codec_flac` | `f434a906…ef3abf1a` | identical |
| FLAC, 16-bit 5.1 at 48 kHz | `demux_flac` + `codec_flac` | `5c449afc…621c8a91` | identical |
| **FLAC, 32-bit 44.1 kHz mono** | `demux_flac` + `codec_flac` | `beea7eb2…7b0abbf0` | identical |
| **FLAC, 16-bit 37800 Hz mono** | `demux_flac` + `codec_flac` | `42673449…97997f86` | identical |
| **FLAC, 16-bit 768000 Hz mono** | `demux_flac` + `codec_flac` | `2fe0b8f6…67b5bfaf` | identical |
| MP3, 44.1 kHz stereo 256k | `demux_mpeg` + `codec_mp3` | `c86e36b4…c49d6482` | identical |
| AAC-LC raw ADTS, 44.1 kHz | `demux_adts` + `codec_aac` | `3b261164…6d516a6b` | identical |
| ALAC, 16-bit 44.1 kHz stereo | `demux_mp4` + `codec_alac` | `b38bebc6…1af5b059` | identical |
| ALAC, 24-bit 96 kHz stereo | `demux_mp4` + `codec_alac` | `f434a906…ef3abf1a` | identical |
| ALAC, 16-bit 5.1 at 48 kHz | `demux_mp4` + `codec_alac` | `5c449afc…621c8a91` | identical |
| AAC-LC in M4A, 44.1 kHz stereo | `demux_mp4` + `codec_aac` | `7ca728d3…4baba3d6` | identical |
| Vorbis in Ogg, 44.1 kHz stereo | `demux_ogg` + `codec_vorbis` | `12fec9f3…b77adfaa` | identical |
| Opus in Ogg, 48 kHz stereo | `demux_ogg` + `codec_opus` | `bffaaece…5b0df268` | identical |

Which is what the split promised: **it adds no conversion.** Bit-exactness is a
property of the codec, and a container never had any of it to lose. The three
bold FLAC rows were not generated by the corpus. They were built by hand with the
reference encoder, because they are the three cases where a frame header stops
being ordinary -- and their headers were read back to check that they are:

| File | Frame header | What it says |
|---|---|---|
| 32-bit | `FF F8 C9 0E` | sample-size code **7**, which FLAC 1.4 assigned to 32 bits and `dr_flac` still marks reserved -- it decodes such a file to silence |
| 37800 Hz | `FF F8 CE 08` | rate code **14**: not one of the eleven the four-bit field can name, so the header spells it out in tens of hertz |
| 768000 Hz | `FF F8 C0 08` | rate code **0**: too fast for even that -- sixteen bits of tens of hertz stops at 655350 -- so the frame says "ask STREAMINFO" |

FLAC's range is 1 Hz to 1048575 Hz, carried in STREAMINFO's twenty-bit field,
and the parser reads the whole of it. The eleven-entry table in the frame header
is a shorthand for the common rates and not the format's range, which is a
distinction worth writing down because reading the table as the range is exactly
how a decoder ends up refusing a file that is perfectly legal.

### Two formats gained a reader, and one of them had been waiting

**MPEG layer II.** `decode_mp3`'s probe tested for Layer III explicitly, so an
MP2 went to FFmpeg and, without FFmpeg, to Media Foundation. The layer is two
bits of the frame header and `dr_mp3` decodes all three, so `demux_mpeg` names
MP_CODEC_MP1, MP2 or MP3 and `codec_mp3` takes any of them.

Measured against the audio that was encoded, with `compare`: 88704 frames, which
is exactly what FFmpeg reports, and **11.99 dB** from the source -- where FFmpeg
also gets 11.99 dB. The two decoders agree with each other to **68.51 dB**, which
is the number that says they are two implementations of one codec rather than
one of them being wrong. The encoder's own loss is what the 12 dB is, and it
swamps the difference between the two decoders by fifty-six decibels. MP2 carries
no gapless tag, so the encoder's 481-frame delay is in the audio and `compare`
reports it as an alignment failure -- correctly, because nothing in the file says
to trim it.

**OggFLAC**, which is the one the Ogg split promised in step 4 and could not
keep until `codec_flac` existed. `demux_ogg` had been reading the container and
naming the codec since then, with nothing to hand it to. It now decodes through
libFLAC to `b38bebc6…1af5b059` -- the same hash as the WAV, the FLAC and the
ALAC of the same audio, and the same hash FFmpeg produces.

### Seeking, which stopped being each decoder's business

v1 gave each decoder a `seek` and hoped. v2 splits it: a demuxer seeks to the
packet holding a frame -- or, for a codec that needs history, to one before it --
and the host discards what precedes the target. That is written once, in
`mp::PacketSource`, instead of once per module.

The demuxer says which packets carry a real position with `MP_PACKET_TIMED`,
because zero is a legitimate position and a host cannot tell "the start of the
stream" from "I do not timestamp" by looking at the number. Ogg is the case that
needs the flag: it timestamps *pages*, not packets, and the granule it stores is
where a page *ends* -- so only the first packet of a page has a position at all,
and it is the page before that supplies it.

Measured by seeking to the same sample in four unrelated framings holding the
same audio, which must therefore produce the same bytes:

| Seek to | WAV | FLAC | ALAC in MP4 | FLAC in Ogg |
|---|---|---|---|---|
| frame 0 | `b38bebc6…` | `b38bebc6…` | `b38bebc6…` | `b38bebc6…` |
| frame 1000 | `b1a61cb2…` | `b1a61cb2…` | `b1a61cb2…` | `b1a61cb2…` |
| frame 44100 | `4f4e25bd…` | `4f4e25bd…` | `4f4e25bd…` | `4f4e25bd…` |
| frame 44101 | `5e264f8b…` | `5e264f8b…` | `5e264f8b…` | `5e264f8b…` |

PCM seeks exactly, FLAC to a 4096-sample block, ALAC to its own, and Ogg to a
page -- and all four land on the same sample. For the lossy formats the same
seek is checked by frame count, which is the strongest statement available when
there is no reference to be identical to: MP3, MP2, AAC, Vorbis and Opus each
return exactly the file's length minus the seek.

Two of those needed the mechanism to be got right rather than merely present:

- **MP3's bit reservoir reaches backwards**, so `demux_mpeg` hands back two
  frames before the target and the host throws them away. The first of those
  decodes to *nothing* -- dr_mp3 declines a frame it has no reservoir for --
  which is why the discard is counted from the first packet that produced
  samples rather than the first that arrived. Counting from the wrong one put
  every MP3 seek 601 frames short.
- **AAC's windows overlap by half**, so `demux_adts` and `demux_mp4` hand back
  one frame of pre-roll for the same reason.

## Where each format goes, and why Media Foundation is last

Audited with `mediaperch-probe claims`, which prints every decoder's probe score
for one file. Score decides; priority only breaks a tie; the first candidate that
actually opens the file wins. `demux_ffmpeg` scores **0** on everything when
`ffmpeg` and `ffprobe` are not installed, which is what makes the last column
meaningful.

| Container / codec | Who claims it, best first | Chosen | With no FFmpeg |
|---|---|---|---|
| WAV — RIFF/WAVE | **`demux_wav` 100**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_wav` | `demux_wav` |
| RIFX, RF64, AIFF, AIFC, W64 | **`demux_wav` 100** | `demux_wav` | `demux_wav` |
| FLAC | **`demux_flac` 100**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_flac` | `demux_flac` |
| Ogg — Vorbis, Opus or FLAC | **`demux_ogg` 100**, then `demux_ffmpeg` 100 | `demux_ogg` | `demux_ogg` |
| Ogg — Speex | **`demux_ogg` 100** — reads it, and no codec here takes the stream — then `demux_ffmpeg` 100 | `demux_ffmpeg` | *nothing* |
| MP3, frame header in reach | **`demux_mpeg` 100**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_mpeg` | `demux_mpeg` |
| MP3, tag past the 4 KB window | **`demux_mpeg` 60**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_mpeg` | `demux_mpeg` |
| MPEG-1/2 layer I or II | **`demux_mpeg` 100**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_mpeg` | `demux_mpeg` |
| AAC in ADTS | **`demux_adts` 100**, `demux_ffmpeg` 30, `demux_mf` 20 | `demux_adts` | `demux_adts` |
| MP4, M4A | **`demux_mp4` 100**, `demux_ffmpeg` 100 (priority 60), `demux_mf` 20 | `demux_mp4`, and the codec it names | `demux_mp4` where a codec here takes the stream, else `demux_mf` |
| ASF, WMA | `demux_ffmpeg` 30, `demux_mf` 20 | `demux_ffmpeg` | `demux_mf` |
| Matroska, WebM | **`demux_mkv` 100**, `demux_ffmpeg` 100 (priority 60) | `demux_mkv`, and the codec it names | `demux_mkv` where a codec here takes the stream |
| WavPack, Monkey's Audio, DSF, DFF, TTA, Musepack, CAF | `demux_ffmpeg` 100 | `demux_ffmpeg` | *nothing* |

**Every row is a demuxer now**, and the bold entries are the ones that read a
container written in this tree rather than handing the file to a pipeline. A
demuxer is asked first, it says what is inside, and the codec is looked up.
Nothing is tried.

The last column is what happens with no FFmpeg installed, and it is worth
reading: eight of the thirteen rows do not change at all, because what reads
them ships here. The rows that become *nothing* are the long tail, which is what
`demux_ffmpeg` is for.

**A demuxer that claims a container can still decline what is in it, and then
the old list has it.** Two measured cases: an Ogg carrying Speex is read by
`demux_ogg` and no codec here takes the stream, and an MP4 carrying AC-3 is read
by `demux_mp4` and the same. Both fall through to `demux_ffmpeg` and play. (A
QuickTime `.mov` used to be the second example. It is not one any more:
`demux_mp4` reads `.mov` since it was rewritten on Bento4.) That fallthrough is
not the v1 "try them in order" coming back — the demuxer is asked once, on
evidence, and answers about the file rather than guessing at it.

### Reading the codec before choosing the decoder

An MP4 is claimed at 100 by `decode_alac`, `decode_aac` and `decode_ffmpeg`
alike, and the order they are tried in owes nothing to what is actually inside.
The obvious improvement — read the codec, then pick — is right in principle and
**cannot be done from a probe**, for a reason that is a property of the format
rather than of this program: the sample entry that names the codec lives in
`moov`, and `moov` may be at the *end* of the file. FFmpeg writes it there;
`refalac` writes it at the front. A probe sees four kilobytes. Scoring on what
happens to be visible would make the chosen decoder depend on which program
wrote the file.

What happens instead is that `open` reads the real index and declines in about
two seeks, and the host moves to the next candidate. The order of attempts is
codec-blind; the outcome is not.

**Ogg shows the version of this that does work.** `decode_ogg` scans the first
page for `OpusHead` or `\x01vorbis` and claims only those; OggFLAC and Speex get
**0** and fall to `decode_ffmpeg`. That is possible because Ogg puts its
identification header in the first page, always — the information is in reach, so
the probe uses it. The same is true of ADTS, where the profile is in the header
`decode_aac` already parses.

**What was done about it, and it was not a better probe.** `demux_mp4` reads
`moov` wherever it is -- and wherever the `moof`s are, on a fragmented file -- in
`open`, where reading a file is allowed and there is no four-kilobyte window; the stream it reports names its codec, and the codec module
is looked up rather than tried. The MP4 ordering problem above is therefore not
solved, it is *absent* — the question that produced it is no longer asked. What
`claims` prints is now both halves:

When `demux_ogg` was written there was no FLAC codec, and what it printed was:

```
$ mediaperch-probe claims --file x.oga
demux_ogg        100  Ogg (libogg, the reference container)
  stream 0  audio    FLAC     -> nothing here decodes it
demux_ffmpeg     100  FFmpeg (found at run time, not shipped)
  stream 0  audio    internal -> this module, itself
```

That last arrow is the difference the split was for. Under `decode_ogg` an
OggFLAC was "an Ogg this module cannot read", which is a fact about the module;
here it is a container that was read, a stream that was identified, and a codec
nobody had written -- a fact about the file, naming exactly what was missing.

**It was then written**, one step later, for a different container entirely:
`codec_flac` exists because `demux_flac` needed it. Nothing about `demux_ogg`
changed and the same command now says `-> codec_flac`. That is what naming the
gap instead of refusing the file is worth. Speex still says `Speex` in the same
place and still plays through `demux_ffmpeg`, and if a Speex codec is ever
written it will arrive the same way.

So the rule is not "MP4 is special": it is that a probe claims on what the first
four kilobytes actually prove, and different containers prove different amounts.

### What claims what, measured across eighteen containers

Asked with `mediaperch-probe claims` on files built for the purpose. Three
answers were wrong, and two of the three cost playback:

| Container | Before | After |
|---|---|---|
| AC-3, E-AC-3 | **nobody** | `ffmpeg` 100 |
| MPEG-TS | **nobody** | `ffmpeg` 100 |
| DTS | `mp3` 100 → refused, and nobody else | `ffmpeg` 100 |
| AMR | `mp3` 100 → refused, and nobody else | `ffmpeg` 100 |
| FLV | `mp3` 100 → refused, and nobody else | `ffmpeg` 100 |
| WavPack | `mp3` 100, `ffmpeg` 100 | `ffmpeg` 100 |
| MP4, MOV, 3GP | `alac`, `aac`, `mp3` 100, `ffmpeg` 100 | `alac`, `aac`, `ffmpeg` |
| AIFF, Sony Wave64 | `native` 100 | unchanged |
| CAF, TTA, WebM, Speex | `ffmpeg` 100 | unchanged |
| OggFLAC | `ffmpeg` 100 | `demux_ogg` reads it now, and `codec_flac` decodes it |
| AVI | `ffmpeg` 30 | unchanged |

**`decode_mp3` was claiming what it could not read.** Its probe walked eight
kilobytes looking for an MPEG frame header, and eight kilobytes of somebody
else's compressed audio contains one by chance. On DTS, AMR and FLV it was the
*only* claimant, so the file was refused outright rather than reaching FFmpeg —
which reads all three. A claim now needs **two headers a frame apart that agree
about version, layer and sample rate**; a chance sync does not chain. At an
ordinary bit rate a frame is a few hundred bytes, so a real MP3 confirms itself
several times inside the window.

The weak claim that remains is the honest one: an ID3v2 tag larger than the
window proves the audio is out of reach rather than absent, and that still scores
60.

**The same audit, run again on the v2 modules, found the wart had a twin.**
`demux_adts` was written with `demux_mpeg`'s shape and inherited its ID3
speculation, so an MP3 with cover art was claimed at 60 by both -- and
`demux_adts` won the tie on priority, opened the file, failed, and let the host
fall through. The right answer by the wrong route. An ADTS stream behind a large
tag is rare enough that the guess costs more than it buys, so that module claims
nothing there; `demux_mpeg` keeps its 60 because a tagged MP3 is most MP3s.
Measured with an ID3v2 tag of eight kilobytes stitched in front of the corpus's
MP3: one claimant now, and the same hash out.

**FFmpeg's magic table here was simply short.** AC-3, E-AC-3 and MPEG-TS had
no claimant at all — files it reads perfectly, refused by the whole tree because
its probe is a table of magic bytes and these were not in it. The alternative is
running `ffprobe` on every candidate, which is a process per file per probe; the
table is the right shape and now also carries AC-3, DTS in both byte orders, AMR,
FLV, Shorten, TAK and OptimFROG, plus MPEG-TS, whose marker is a 0x47 byte every
188 bytes rather than a prefix.

Six formats went from *refused by everything* to playing: AC-3, E-AC-3, MPEG-TS,
DTS, AMR and FLV. Shorten, TAK and OptimFROG are added on their documented magic
and **not measured** — nothing here can encode them.

One capability turned up that nobody had written down: **dr_wav reads
Sony Wave64**, because `dr_wav` does and its magic begins with the same four
bytes the probe already looked for.

### Media Foundation takes one score, and it is the lowest

It used to claim 100 on MP4, on any MPEG frame header and on ASF, and win two of
those outright. Every one of its formats has since been measured, and every
measurement went the same way — they are all in this document:

- float WAV comes back **clipped** to 32-bit integer, 73.8% of the samples pinned;
- a 32-bit FLAC is **refused**, at a rate it reads happily at 24 bits, and so is
  FLAC above about 655 kHz;
- multichannel ALAC comes back with every sample perfect and **the channels in the
  wrong speakers**;
- gapless metadata is implemented in **no codec at all**: every MP3 starts 36 ms
  late, every AAC 1024 frames late with the encoder padding left on;
- 8 kHz and 7.1 AAC are refused;
- a stream that declares no depth is decoded to **16 bits**, which for a lossy
  codec is a quantisation nobody asked for. The generated matrix shows it: WMA
  comes out `S16` from Media Foundation and `F32` from everything else.

There is no format here where it is the best answer and several where it is
measurably the worst. So it now scores **20** on everything it recognises — one
below `demux_ffmpeg`'s own fallback score of 30 — and the recognition list is
unchanged.

What that buys is the thing this module is actually for. On a machine with
nothing installed, `demux_ffmpeg` scores 0 and 20 beats nothing at all, so
Media Foundation still plays WMA, still plays MPEG layer II, and is still the
last resort for MP4. **It is the floor, not a competitor.**

Two rows changed hands: WMA and MPEG layer I/II now go to FFmpeg wherever it is
installed. WMA gains float instead of a silent quantisation to 16 bits.


### The long tail, measured

| File | Chosen decoder | Reported | Note |
|---|---|---|---|
| Vorbis in Ogg | `demux_ogg` + `codec_vorbis` | `44100 Hz / 1 ch / F32` | libvorbis itself, driven directly rather than through vorbisfile |
| Opus in Ogg | `demux_ogg` + `codec_opus` | `48000 Hz / 1 ch / F32` | libopus, likewise. Opus decodes at 48 kHz whatever the source rate was; that is the codec, not a resample |
| OggFLAC | `demux_ogg` + `codec_flac` | `44100 Hz / 2 ch / S16` | it used to be FFmpeg's, because `decode_ogg` scored it **0** -- not something low, but "this is an Ogg I cannot read at all". `demux_ogg` reads the container and names the codec; `codec_flac` takes it. Byte-identical to the FLAC and the WAV of the same audio |
| WavPack | `demux_ffmpeg` | `44100 Hz / 2 ch / S32` | hash identical to the 32-bit WAV of the same signal |
| ALAC in M4A | `demux_mp4` + `codec_alac` | up to `384000 Hz / 8 ch / S32` | both halves ours, and between them no dependency at all. See below |
| MP3 | `demux_mpeg` + `codec_mp3` | `44100 Hz / 2 ch / F32` | `dr_mp3`, at 105. It outranks Media Foundation because MF implements no gapless metadata and starts every MP3 36 ms late -- see *MP3, and the 36 milliseconds* below. This row once said `mf` and `S16`, which is what a hand-written table does |

## Vorbis and Opus: float, and what that costs

`codec_vorbis` and `codec_opus` report `F32`, because libvorbis and libopus produce float and a
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
L,C,R,BL,BR,LFE where WAVE 5.1 is L,R,C,LFE,BL,BR. The two codecs permute, which
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

### Why not just hand these to Windows?

It is a fair question and the right one to ask, because these codecs are lossy:
there is no bit-exactness to protect, so the usual argument for the reference
implementation does not apply. Four submodules is a real cost. The answer is
that Media Foundation cannot do the job, and it fails in ways that were only
visible once measured.

**It cannot open Ogg at all.** Not the codec — the container. Every `.ogg` and
`.opus` file in the test set is refused outright by `MFCreateSourceReaderFromURL`.
Windows has the Vorbis and Opus *decoders*; it has no Ogg demuxer. Put the same
streams in Matroska or WebM and MF decodes them happily. Since virtually every
Vorbis and Opus file in the world is in Ogg, using MF would mean writing an Ogg
demuxer and feeding it packets through `IMFTransform` — more of our code, not
less, and libvorbis still not replaced.

For the files it *can* open, the differences are these. Each row is Matroska,
and the FFmpeg column is FFmpeg reading the **same file** MF was given, which is
what makes the length rows a statement about MF rather than about the container:

| | ours | FFmpeg | Media Foundation |
|---|---|---|---|
| Vorbis, content | reference | 132 dB | **131 dB** — as good as anyone's |
| Vorbis, 7.1 channel order | correct | correct | **correct** |
| Vorbis, length (three files) | 384000 / 96000 / 16000 | same | **384576 / 96832 / 16128** |
| Opus, content after the head | reference | 134 dB | **134 dB** |
| Opus, first 648 samples | correct | correct | **wrong**: peak error 0.48 on a 0.60 signal |
| Opus, length | 288000 | 288000 | **288648** |
| Opus, 7.1 | decoded | decoded | **refused** |
| Output | F32, the codec's own | F32 | S32 or S16 |

Read the Opus row carefully, because it is the one that matters most. From
sample 648 onward MF agrees with the reference to 1.5 × 10⁻⁷ — its decoder is
fine. What it does not do is honour the pre-skip that the stream carries and the
specification requires it to discard. The result is **13.5 ms of wrong audio at
the start of every Opus track** and an untrimmed tail, which is audible at a
track boundary and makes gapless playback impossible. FFmpeg, given the same
Matroska file with the same `CodecDelay`, produces exactly the right length.

The Vorbis length rows are the same failure in a milder form: 128 to 832 extra
samples depending on the file, where FFmpeg reading that file is exact.

One thing this measurement did improve. Media Foundation used to be asked for 16 bits when
a stream declares no depth of its own, which every compressed stream does. Every
lossy codec here decodes to float, so that was a quantisation performed inside
our own decoder, silently. It asks for 32 now: Opus in MP4 went from `S16` to
`S32`, and AAC still comes back `S16` because Media Foundation's AAC decoder
will not produce more, which is the fall-back working rather than a regression.

### The edges of each format

| File | ours | FFmpeg | MF |
|---|---|---|---|
| Vorbis, 192 kHz stereo | `192000 Hz / 2 ch / F32` | same | padded |
| Vorbis, 8 kHz stereo | `8000 Hz / 2 ch / F32` | same | padded |
| Vorbis, 7.1 at 48 kHz | `mask 0x63f` | no mask | correct order, padded |
| Opus, 7.1, mapping family 1 | `mask 0x63f` | decoded | **refused** |
| Opus, 12 and 16 channels, mapping family 255 | **refused** | decoded | refused |

Opus allows up to 255 channels through mapping family 255. `codec_opus` stops at
eight, because eight is where WAVE channel masks stop and a decoder here does not
report a layout it cannot name. Refusing hands the file to `demux_ffmpeg`, which
decodes it — the fallback chain doing exactly what it is for, and the reason
refusing is an acceptable answer rather than a dead end.

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
`demux_ffmpeg` remains one command away for anyone who disagrees, which is the
point of the module boundary.

## ALAC, decoded here -- in Rust

ALAC is the one codec this tree decodes with no dependency of any kind: no
submodule, no runtime library, no OS codec. The decoder is
`modules/codec/alac/decoder`, a crate of its own, and it is the first module in
the tree that is not C++. [The plan](plan.md) §7 has the argument for writing it at all; the
short version is that Apple's reference implementation is simultaneously the
specification and unmaintained since 2011, and the second half of that is what
ALHACK was. §2 has the argument for the language, and the reason it was taken
up for this module first.

### What changed in the move to Rust, and what did not

**Not a sample.** The decoder is a port of the C++ one, itself written from the
reference read as a specification, and the port was accepted on one criterion:
every hash this document already records had to come out the same. They did.

| File | Through | Recorded | From Rust |
|---|---|---|---|
| ALAC, 16-bit 44.1 kHz stereo | `demux_mp4` + `codec_alac` | `b38bebc6…` | **identical** |
| ALAC, 24-bit 96 kHz stereo | `demux_mp4` + `codec_alac` | `f434a906…` | **identical** |
| ALAC, 16-bit 5.1 at 48 kHz | `demux_mp4` + `codec_alac` | `5c449afc…` | **identical** |
| ALAC in Matroska | `demux_mkv` + `codec_alac` | `b38bebc6…` | **identical** |
| ALAC in QuickTime `.mov` | `demux_mp4` + `codec_alac` | `b38bebc6…` | **identical** |

Seeking too: frames 0, 1, 1000, 44100, 44101 and 88199 all match the source WAV
seeked to the same frame, as they did before. And the ceiling of the format,
re-encoded by hand with Apple's reference encoder and decoded back:

| Channels | Rate | Depth | Reported | Against the source WAV |
|---|---|---|---|---|
| 2 | 48000 | 32 | `S32` | **identical** |
| 2 | 96000 | 24 | `S24_PACKED` | **identical** |
| 6 | 48000 | 24 | `S24_PACKED`, mask `0x3f` | **identical** |
| 8 | 48000 | 16 | `S16`, mask `0xff` | **identical** |
| 8 | 96000 | 24 | `S24_PACKED`, mask `0xff` | **identical** |
| 8 | 384000 | 32 | `S32`, mask `0xff` | **identical** |

(The reference encoder refuses ffmpeg's `7.1` layout, which has side channels;
ALAC's eight-channel layout is `7.1(wide)`, with front-centre pairs, and that is
what those rows are. The channel permutation the next section is about is
exercised by every row past stereo.)

**What did change is what a missed check costs.** In C++ the five things Apple's
decoder does not check -- the table under *What was written instead of linked*
-- were five bounds checks somebody had to think of, and the FLAC parser that
used to sit beside this decoder showed what happens when one is not thought of:
`flacframe_fuzzer` found an out-of-bounds read in it in ninety seconds. In Rust
an index past a slice is a panic; `mp-abi` catches the panic at the module
boundary, logs it, and returns `MP_ERR_INTERNAL`, and the host sees a packet
that failed to decode. The class of bug is gone rather than guarded against.

**Where the `unsafe` went.** The ABI is C -- raw pointers and lengths -- so a
module cannot cross it without `unsafe`, and the question was only where to put
it. It is in `modules/shared/mp-abi`, once: turning `(ptr, len)` into a slice,
handing a `Box` across as an opaque handle and getting it back, and calling the
host's `log` through its function pointer. Nineteen `unsafe` blocks, two
`unsafe fn` and two `unsafe impl Sync`, every one of those three shapes, all in
one file. The decoder is its own crate and carries `#![forbid(unsafe_code)]` at
crate level, which the compiler enforces; the module glue carries
`#![deny(unsafe_code)]` with one `allow` for the `#[no_mangle]` on
`mp_module_entry`, which Rust counts as unsafe because a colliding symbol is. `abi/probe_rust` measured that a panic can
be contained at this boundary before any of this was written.

**Arithmetic is wrapping where the C++ relied on it.** ALAC's adaptive Golomb
parameters are tuned around unsigned 32-bit wraparound and the predictor's sums
overflow on hostile input; in C++ that was implicit (and, for the signed sums,
undefined). In Rust each such site says `wrapping_*`, and the crate's tests run
under the dev profile with overflow checks on, so any site that was missed is a
test failure rather than a difference in a release build.

**One hardening the C++ did not have.** `mix_bits` is a byte from the stream and
a shift count; a value of 32 or more is an undefined shift in the reference and
was a masked one in the C++ (x86 masks the count). A real encoder writes 2. The
Rust refuses the packet.

The fuzzer moved with the decoder and kept its corpus: `fuzz/corpus/alac` seeds
`modules/codec/alac/fuzz` exactly as it seeded `alac_fuzzer.cpp`, with the same
input shape. It is coverage-guided on the stable toolchain -- LLVM's
SanitizerCoverage pass reached through `-C passes=sancov-module`, libFuzzer
supplying the `__sanitizer_cov_*` symbols and a one-page C shim supplying the
section sentinels a COFF linker will not -- which is written up in
`docs/building.md` because it is not the documented way to fuzz Rust and took
four link errors to find. Measured: 608 counters in the decoder, and a five-minute
campaign -- 1,304,111 executions, 3,286 new corpus entries, 37 MB peak -- that
found nothing, which for a port is the second result that matters.

### Bit-exact to the ceiling of the format

ALAC tops out at 32 bits, 384 kHz, 7.1. Every row here was encoded from the WAV
in the first column with `refalac` — Apple's own reference *encoder*, so the
files are not FFmpeg's idea of ALAC — and decoded back. (This table is the C++
decoder's, kept as the record it was; the Rust decoder's rows are above and
agree with every one of these it repeats.)

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

Probing reads four kilobytes. Opening reads the whole header, and under v2 it
also reads what is inside: a demuxer says which codec each stream carries, and
the codec is asked whether it takes that stream. So a module can score highest
and still refuse, and several do it for good reasons — `demux_mp4` on a
QuickTime sample entry it does not implement, `demux_ogg` on an Ogg carrying
Speex, `codec_aac` on every AAC profile past LC. The refusal names the codec
rather than the module, which is the difference the split made to this
paragraph.

The registry therefore ranks every decoder that claims the file and the host
walks the list, so a refusal costs the next candidate rather than the file:

| File | Ranked | Opens | Used |
|---|---|---|---|
| 32-bit FLAC | flac, native, mf, ffmpeg | flac | `decode_flac` |
| 7.1 ALAC | alac, aac, ffmpeg, mf | alac | `decode_alac` |
| AAC-LC in M4A | alac, aac, ffmpeg, mf | aac | `decode_aac` |
| 8-channel AAC | alac, aac, ffmpeg, mf | aac | `decode_aac` |
| HE-AAC in M4A | alac, aac, ffmpeg, mf | ffmpeg | `decode_ffmpeg` |
| Vorbis | ogg, ffmpeg | ogg | `decode_ogg` |

Four modules score 100 on an MP4 and the first three rows resolve to three
different ones, which is the mechanism working rather than a cost. `decode_alac`
scores 100 on every MP4, not only the ones with `alac` visible in the first four
kilobytes —
because whether it is visible depends on where the muxer put `moov`, and FFmpeg
puts it last while `refalac` puts it first. Scoring on that would make the chosen
decoder depend on which program wrote the file. Looking first costs two seeks and
a few hundred kilobytes; the alternative costs the ability to reason about it.
`decode_aac` does the same, and refuses at the same point: an ALAC track is not
an `mp4a` track, and an AudioSpecificConfig that names object type 5 or 29 is
HE-AAC and belongs to FFmpeg.

`--decoder` does not get a fallback. Being told "use that one" and answering
with a different one is not an answer; a forced decoder that refuses reports the
refusal.

## MP3, and the 36 milliseconds

`decode_mp3` exists because of one measurement, not because MP3 needed a better
decoder. Media Foundation does not implement gapless metadata: it returns 2544
frames more than the file holds and starts the audio **1729 frames — 36.0 ms —
late**, presenting the encoder's warm-up as audio. `dr_mp3` reads the LAME/Xing
tag, skips the delay, stops at the padding boundary, and subtracts the delay
from its frame count.

It also cost nothing. `dr_mp3` lives in `external/dr_libs`, the submodule
`decode_native` already uses, so the module is one source file and an
implementation unit.

### Lengths, against the file that was encoded

| File | source | `decode_mp3` | `decode_ffmpeg` | `decode_mf` |
|---|---|---|---|---|
| 48 kHz stereo, 320 kbps | 96000 | **96000** | 96000 | 98544 |
| 8 kHz mono, 8 kbps (MPEG-2.5) | 16000 | **16000** | 16000 | 17328 |
| 44.1 kHz VBR V0 | 88200 | **88200** | 88200 | 89328 |
| 44.1 kHz dual channel | 88200 | **88200** | 88200 | 90480 |
| pink noise, 320 kbps | 96000 | **96000** | 96000 | 98544 |

Measured against pink noise, `decode_mp3` and `decode_ffmpeg` both start at
**+0 frames** from the source. And on a file with no gapless tag at all, the two
agree at 97920 frames — there is nothing to trim there, and neither invents any.

### Content, against FFmpeg's own decoder

| File | Identical samples | SNR | max abs difference |
|---|---|---|---|
| 48 kHz, 320 kbps | 6.7% | **124.24 dB** | 4.4 × 10⁻⁷ |
| 8 kHz mono, 8 kbps | 5.7% | **125.25 dB** | 2.2 × 10⁻⁷ |
| 44.1 kHz VBR V0 | 6.1% | **124.47 dB** | 3.9 × 10⁻⁷ |

MP3 has no normative bit-exact decoder — ISO 11172-4 defines conformance as an
RMS error bound — so two implementations are not expected to agree at all. These
agree to float rounding.

Every MPEG version and rate was checked: MPEG-1 at 44.1 and 48 kHz, MPEG-2 at
16, 22.05 and 24 kHz, MPEG-2.5 at 11.025 and 12 kHz, CBR, VBR and dual channel.
Lengths match FFmpeg exactly in all of them. Seeking matches the tail of a
straight decode at 1152, 10000 and 50000 frames — and FFmpeg's seeks match
FFmpeg's own tails, so both are self-consistent and differ only by the 124 dB
that separates the decoders anyway.

The output is `F32`, and the build defines `DR_MP3_FLOAT_OUTPUT` to make that
true rather than nominal: without it dr_mp3 decodes to int16 and converts up on
the way out, which would be a quantisation performed inside a decoder.

`fuzz/mp3_fuzzer.cpp` drives it: 1.5 million executions under ASan, nothing
found. MP3 earns a fuzzer more than the others here, because its frame parser
resynchronises after garbage and will keep decoding through a file that is
mostly not MP3.

## AAC-LC, written here

`decode_mp3` exists because Media Foundation got one thing wrong. `decode_aac`
exists because every AAC decoder that could have been used got something wrong,
and each a different thing:

| | What it does | Why that ended it |
|---|---|---|
| Media Foundation | starts 1024 frames — **21.3 ms** — late, refuses 8 kHz and 7.1 | no gapless metadata in any codec, and two formats it will not open |
| FAAD2 | discards **two** frames where the file says one | output begins 1024 frames into the audio. Four ways of driving it gave the identical wrong placement |
| libxaac | 16- or 24-bit **integers** only | AAC's inverse transform produces real numbers; taking integers is a quantisation performed where nobody can see it |
| FDK-AAC | — | the licence is not GPL-compatible, so the question stops there |

So the codec is in this tree, next to ALAC, for the reason §7 of
[the plan](plan.md) gives: write it yourself when the codec is small enough that
you can. It is 1,599 lines, plus 368 of generated tables. **SBR and PS are not
here and are not planned** — they are another six thousand lines apiece, and an
AudioSpecificConfig that asks for object type 5 or 29 is refused at the door so
the file goes to `decode_ffmpeg`, which is what the fallback chain is for.

### Lengths, against the file that was encoded

Thirty-three files: every sample rate AAC-LC defines, channel configurations 1 to
6 and 8, bit rates from 8 to 512 kbps, durations from 0.02 s to 10 s, in MP4 and
in raw ADTS. A selection, and the rule the rest follow:

| File | source | `decode_aac` | `decode_ffmpeg` |
|---|---|---|---|
| 8 kHz stereo | 8000 | **8000** | 8000 |
| 96 kHz stereo | 96000 | **96000** | 96000 |
| 5.1 at 48 kHz | 48000 | **48000** | 48000 |
| 7.1(wide) at 48 kHz, 8 channels | 47104 | **47104** | 47104 |
| 44.1 kHz stereo at 8 kbps | 44100 | **44100** | 44100 |
| 0.02 s stereo | 960 | **960** | 960 |
| 10 s stereo | 480000 | **480000** | 480000 |
| raw ADTS, 44.1 kHz stereo | 44100 | 46080 | 46080 |

**Every MP4 row equals the source exactly**, because the file says how much of
the decoded audio it claims and this module reads the `elst` edit list that says
it. The ADTS row is the control: a raw stream carries no such information
anywhere, so the encoder's delay and padding stay in and a *longer* result is the
correct one — both decoders keep the same 1980 extra frames, and neither invents
a trim the file did not ask for.

### Content, against FFmpeg's own decoder

| File | SNR | max abs difference |
|---|---|---|
| 48 kHz mono | **134.86 dB** | 8.9 × 10⁻⁸ |
| 8 kHz stereo | **135.82 dB** | 6.7 × 10⁻⁸ |
| 96 kHz stereo | **134.76 dB** | 7.5 × 10⁻⁸ |
| 5.1 at 48 kHz | **133.66 dB** | 8.9 × 10⁻⁸ |
| 7.1 at 48 kHz | **134.05 dB** | 8.9 × 10⁻⁸ |
| 7.1(wide), channel configuration 0 | **135.89 dB** | 7.5 × 10⁻⁸ |
| raw ADTS 44.1 kHz | **134.38 dB** | 7.5 × 10⁻⁸ |

Across the whole corpus — those thirty-three plus a 30-second file and a
two-packet one — the range is **134.5 to 140.0 dB**, and the largest
single-sample difference anywhere in any of them is **8.9 × 10⁻⁸** — −141 dBFS,
below one LSB of a 24-bit sample, which is −138.5.

Identical bytes are not the target here and could not be. AAC has no normative
bit-exact decoder: ISO/IEC 14496-4 defines conformance as an RMS error bound, and
the inverse transform is an algorithm choice — a direct cosine sum accumulated in
`double` here, a split-radix FFT in `float` there. Two correct decoders differ by
float rounding and that is what 135 dB is.

Two places where they agree only because this decoder was made to:

- **Noise substitution.** A band coded with codebook 13 has its *energy* fixed by
  the file and its contents left to the decoder, so two conformant decoders put
  different noise there and no comparison between them can be sample-exact. This
  decoder uses FFmpeg's generator and FFmpeg's seed for exactly one reason: it
  makes the rest of the decoder checkable against FFmpeg to the last bit. It is a
  testing convenience and not a claim about the format.
- **Channel order.** AAC's elements yield 5.1 as C, L, R, Ls, Rs, LFE — centre
  first, because the single-channel element comes first. WAVE wants L, R, C, LFE,
  Ls, Rs. Without the permutation a 5.1 decode measures −3 dB against a reference
  while every individual channel is perfect.

**MSVC and clang-cl produce identical output**, hash for hash, on every file
above. Two compilers agreeing is not a proof of correctness, but a decoder whose
result depends on which one built it has an undefined shift or an
order-of-evaluation problem in it somewhere, and this says there is not one.

### Bit accounting

Every frame is checked a second way, and this one needs no reference decoder. A
frame that parsed correctly ends *inside the last byte of its packet*; a parser
that has gone wrong almost never does. Across the whole corpus — **36 files, 3567
packets — every frame ends exactly on its packet boundary, with zero slack
bits**. A single misread bit desynchronises a frame, and it cannot land on the
boundary again by luck.

### Configuration 0, and where the layout hides

Eight channels was the last thing to work, and not for a reason that had anything
to do with decoding. FFmpeg's encoder writes `channel_configuration = 0` for
7.1(wide) and puts the layout in a `program_config_element` — in the
AudioSpecificConfig for an MP4, in every frame for raw ADTS. A PCE gives counts
and pair flags rather than positions, and its front elements are listed **from
the centre outwards**: the leading single element is the centre, and where there
are two pairs the *first* is front-left/right-of-centre and the second is the
main left and right pair. Reading those the other way round decodes all eight
channels perfectly and puts two of them in the wrong speakers — it measures
−0.09 dB overall while every channel matches some FFmpeg channel at 136 dB, which
is what pointed at the answer.

Both places the element can live are exercised: the same eight channels muxed as
raw ADTS, where the PCE arrives inside every frame instead, decode to **135.89
dB** as well.

## WAV, across every axis it has

WAV is the only format here with no compression to argue about, so agreement
means identical SHA-256s and nothing weaker. Every cell below is one:

**Channel counts**, 24-bit at 48 kHz — `decode_native`, `decode_mf` and
`decode_ffmpeg` produce the same hash for **1, 2, 3, 4, 6, 7, 8, 12, 16 and 24
channels**. The reported masks follow the file: `0x3f` at six, `0x63f` at eight,
`0x2d63f` at twelve, and **no mask at sixteen and twenty-four**, which is honest
— WAVE runs out of named speaker positions before it runs out of channels, and a
decoder here does not invent one.

**Sample rates**, 24-bit stereo — the same three agree at **8 kHz, 44.1, 96,
384, 768 kHz and 1.536 MHz**.

**Bit depths**, stereo at 48 kHz:

| Depth | `decode_native` | `decode_mf` | `decode_ffmpeg` |
|---|---|---|---|
| **8-bit unsigned** | `U8` | refused | refused |
| 16 | `S16` = | = | = |
| 24 | `S24_PACKED` = | = | = |
| 32 | `S32` = | = | = |
| **32-bit float** | `F32` = | **`S32`, clipped** | `F32` = |
| **64-bit float** | `F64` | refused | `F32`, with a warning |

Two rows are not agreement, and both are worth reading.

### Float WAV: Media Foundation clips it

A float WAV can hold values above ±1.0 — that is what float WAV is *for*, and
mixing engines produce them routinely. Given a file peaking at **2.5**:

| | Reported | Peak returned |
|---|---|---|
| `decode_native` | `F32` | **2.5000** |
| `decode_ffmpeg` | `F32` | **2.5000** (identical hash) |
| `decode_mf` | `S32` | **1.0000** |

Media Foundation converts float WAV to 32-bit integer and pins everything above
unity. In that file **73.8% of the samples were above 1.0, and Media Foundation
returned every one of them at exactly full scale** — its output matches a clipped
copy of the source to 138.5 dB and matches a scaled copy to 7.5 dB, so it is
clipping and not gain. This is a lossless format being altered, which is why
`decode_native` scores 100 on WAV and `decode_mf` scores 40.

Media Foundation refuses 64-bit float outright, which is the honest answer.
`decode_ffmpeg` narrows it to `F32` and now says so:

```
[warn ] ... holds 64-bit floats and is being narrowed to 32; the output is not
        the file's own samples
```

### Eight bits, sixty-four bits, and four containers

Both of those rows used to read *refused*, and neither refusal was dr_wav's.
`drwav_read_pcm_frames` hands back the file's own bytes unconverted, so it had
been able to read 8-bit and 64-bit float all along; what was missing was a type
in `MpFormat` to name them with. There are two now, and both are Path B only,
because no endpoint accepts either width:

- **`MP_SAMPLE_U8`** — 8 bits in one byte, and the only **unsigned** type here.
  Silence is 128, not 0. That is WAV's convention and it is why this needs its
  own type rather than a quiet bias into `S16`: the bias would be a conversion.
- **`MP_SAMPLE_F64`** — IEEE double. No audio hardware in existence takes
  64-bit samples, so this can never be a wire format. It exists so a decoder can
  say what the file holds; the narrowing then happens in the graph, where
  somebody chose it.

Neither reaches negotiation: candidates are generated over the 2-, 3- and 4-byte
integer containers, and `canonical_for` returns `none` for one and eight.

The same run found something else free. dr_wav reads **RIFF, RIFX (big-endian),
RF64 (past four gigabytes), W64 and AIFF**, and has for as long as this module
has existed — but the probe only ever claimed `RIFF`+`WAVE`, so nothing else
could be reached except by `--decoder native`. It claims all five now. One
signal, four containers, one hash:

| Container | SHA-256 |
|---|---|
| RIFF | `f2d0870d1c3e673d…` |
| AIFF | `f2d0870d1c3e673d…` |
| RF64 | `f2d0870d1c3e673d…` |
| W64 | `f2d0870d1c3e673d…` |

AIFF is big-endian, so that row is dr_wav byte-swapping an entire file into
agreement with the little-endian one, sample for sample.

A-law, µ-law and ADPCM stay refused. dr_wav decodes them, but only by expanding
them to 16 bits, and that is a conversion.

## MP3 and AAC, and the thing Media Foundation does not do

Neither is lossless, so the comparison is length, start alignment and content
rather than hashes. The source in each row is two seconds — 96,000 frames — and
the alignment was measured against pink noise, because a steady tone gives a
correlation peak every period and cannot answer the question:

| File | source | `decode_ffmpeg` | `decode_mf` |
|---|---|---|---|
| MP3, 48 kHz 320 kbps | 96000 | **96000**, starts at +0 | 98544, **starts 1729 frames late (36.0 ms)** |
| MP3, 8 kHz mono 8 kbps (MPEG-2.5) | 16000 | **16000** | 17328 |
| MP3, 44.1 kHz VBR V0 | 88200 | **88200** | 89328 |
| MP3, 44.1 kHz dual channel | 88200 | **88200** | 90480 |
| AAC, 48 kHz 320 kbps | 96000 | **96000**, starts at +0 | 97280, **starts 1024 frames late (21.3 ms)** |
| AAC, 96 kHz stereo | 192000 | **192000** | 193536 |
| AAC, 5.1 at 48 kHz | 96000 | **96000** | 97280 |
| AAC, 8 kHz stereo | 16000 | **16000** | **refused** |
| AAC, 7.1 at 48 kHz | 96000 | **95232** | **refused** |
| AAC, raw ADTS 44.1 kHz | 88200 | 90112 | 90112 |

The last row is the control that makes the rest a finding. Raw ADTS carries no
gapless metadata, so there is nothing to trim and **both decoders agree at
90112** — the difference in every other row is not decoding, it is that
`decode_ffmpeg` reads the gapless information and Media Foundation does not:
the LAME/Xing tag for MP3, the MP4 edit list for AAC, and — from
[the Vorbis and Opus measurements](#why-not-just-hand-these-to-windows) —
`CodecDelay` for Opus.

That is one behaviour across four codecs. Every track decoded by Media
Foundation begins with tens of milliseconds of the encoder's warm-up presented
as audio, and ends with padding the file said to discard:

| Codec | Delay Media Foundation leaves in | Who reads it now |
|---|---|---|
| MP3 | 36.0 ms | `decode_mp3` |
| AAC | 21.3 ms | `decode_aac` |
| Opus | 13.5 ms | `decode_ogg` |

Audible at a track boundary, and enough to make gapless playback impossible.
All four now have a decoder that reads the metadata, which is why the table above
has no "still" left in it.

Two smaller results from the same run. Media Foundation **refuses 8 kHz AAC and
7.1 AAC** outright, which is the honest failure. And its 5.1 AAC channel order is
correct, matching both the source and FFmpeg — so the channel scramble really is
specific to ALAC and not a general multichannel problem.

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

## Against the audio that was encoded

Everything above compares one decoder with another. That answers *do these
agree* and it cannot answer *are they both wrong*, so there is now a second
measurement that holds each decoder against the uncompressed file that went into
the encoder -- the only reference outside every decoder. `mediaperch-probe
compare` makes it, `cmake/DecodeQuality.cmake` drives twelve rows of it, and CI
runs the lot on every push.

**What it can prove is narrower than it sounds.** The encoder threw information
away on purpose: at 256 kbps the decode sits 17 dB from the source, while two
correct decoders sit 134 dB from each other. The encoder's loss is common to
both and about six orders of magnitude larger, so **this cannot rank two
decoders on fidelity** and no amount of care with the thresholds will make it.

What it can do is the part no decoder-to-decoder comparison can:

| Measured | Against | Why it needs the source |
|---|---|---|
| length | the frame count of the file that was encoded | agreeing with FFmpeg about a length says nothing if FFmpeg is wrong too |
| alignment | zero, by cross-correlation | this is the bug Media Foundation has in four codecs and FAAD2 has in one |
| channel order | a correlation matrix, every channel against every channel | two channels swapped is invisible to any broadband figure |
| band energies | the source's own spectrum | a perceptual encoder is *designed* to preserve band energy, so a band that is wrong is a bug where a different waveform is not |
| a fidelity floor | a per-row threshold | it assumes nothing about any other decoder |

The signal is a sweep plus white noise, one distinct pair per channel. The sweep
is there because **a test signal has to be able to locate itself** -- a steady
tone correlates with itself once per period and every peak is a plausible answer,
which is the mistake the MP3 delay measurement made once already. The measurement
reports how far its correlation peak stands above everything outside its own
shoulder, so a signal that cannot answer the question says so instead of
producing a number. The noise is there because it is what makes channels tell
each other apart, and because noise is what drives an AAC encoder to substitute
noise -- a sweep alone never reaches that code path at all.

### The twelve rows

| Row | Length | Start | Channels | Worst band | Against the source |
|---|---|---|---|---|---|
| FLAC, stereo 44.1 kHz | exact | +0 | in order | 0.00 dB | **identical** |
| ALAC, stereo 44.1 kHz | exact | +0 | in order | 0.00 dB | **identical** |
| ALAC, 5.1 at 48 kHz | exact | +0 | in order | 0.00 dB | **identical** |
| MP3, stereo 44.1 kHz 256k | exact | +0 | in order | 0.07 dB | 13.75 dB |
| AAC, stereo 44.1 kHz 256k | exact | +0 | in order | 0.15 dB | 16.57 dB |
| AAC, stereo 44.1 kHz 32k | exact | +0 | in order | — | 0.42 dB |
| AAC, mono 48 kHz 192k | exact | +0 | — | 0.10 dB | 23.40 dB |
| AAC, stereo 8 kHz 64k | exact | +0 | in order | 0.30 dB | 20.38 dB |
| AAC, stereo 96 kHz 512k | exact | +0 | in order | 0.05 dB | 5.05 dB |
| AAC, 5.1 at 48 kHz 768k | exact | +0 | in order | 0.90 dB | 7.04 dB |
| AAC, 7.1(wide) at 48 kHz 1024k | exact | +0 | in order | 0.72 dB | 7.96 dB |
| AAC, raw ADTS stereo 44.1 kHz | *longer* | **+1024** | in order | 0.15 dB | 16.57 dB |

The three lossless rows are the strictest in the file: nothing was thrown away,
so the decode has to *equal* the source, and it does. The last row is the
control. Raw ADTS carries no gapless metadata, so the encoder's delay is
correctly still in the audio -- the decode is a whole frame late and longer than
the source, and both are right. Every other check still applies to it, because
the alignment is found first and the rest is measured where the audio actually
landed. A decode that starts *early* fails whatever the format.

### Does the check work? Three bugs put back

A test that has never failed is a test nobody has reason to believe. Each of the
three real bugs from this milestone was put back into the decoder and the twelve
rows re-run:

| Bug put back | Source-referenced checks | Agreement with FFmpeg |
|---|---|---|
| the program config element's front pairs read outwards-in | **caught** -- channel order, and the alignment collapsed with it | caught |
| the `elst` edit list ignored | **caught** -- 7 of 12 rows, every one on "starts 1024 frames from where the source does" | caught |
| the noise generator not reset by `init()` | **passed every one** | **caught** -- 24 dB where 125 was required |

The third row is the interesting one and it is why both halves of the check
exist. A noise-substituted band is arbitrary by design: the file fixes its
*energy* and leaves its contents to the decoder. So a decoder filling those bands
from the wrong point in its generator produces bands of the right width, in the
right place, at the right energy -- the source cannot see the difference, and
did not. What sees it is holding the two decoders against *each other*, which is
the measurement the source was supposed to replace.

Neither comparison is the better one. They fail in different directions, which is
the only reason to have two.

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
calls that the end of the stream, and the track is skipped without a word. The
answer at the time was for the host to decode one frame and rewind before
declaring an open a success, so a decoder that produced nothing was refused with
a sentence rather than accepted with an empty file:

```
f32_44k.flac    cannot decode: the decoder opened the file and produced no audio at all
```

**The gap was closed by using libFLAC itself**, and it stayed closed through the
v2 split: all three files decode to hashes identical to the reference decoder's,
including at 1,048,575 Hz, through `demux_flac` and `codec_flac`. FFmpeg also
decodes them correctly, so `demux_ffmpeg` would have worked too -- libFLAC is the
smaller answer, builds with CMake, and is the specification for a lossless codec
rather than an implementation of it.

The one-frame check itself is gone with the interface it lived in. What replaced
it is stronger and cheaper: a demuxer names the codec of each stream and the
codec says whether it takes it, so a stream nothing can decode is refused by
name before a byte of audio is asked for. `dr_flac` would decline a 32-bit
STREAMINFO in `probe` today rather than decoding it to nothing.

## 32 bits, measured by hand

The generated matrix in the README stops at 24 bits, and the reason is the
encoder rather than the decoders: FFmpeg's FLAC encoder writes 24 bits when asked
for 32, and integer lossless at 32 bits is where its encoders are least dependable
generally. A generated table whose corpus is wrong measures the corpus.

So these rows are made with reference encoders that are **not** part of this
build and not referred to anywhere in it -- `flac` 1.5.0 from Xiph, and
`refalac` 1.89 for ALAC -- and written down here instead. The source is two
seconds of pink noise at 48 kHz, stereo, quantised to 32-bit integer; the
reference hash is that WAV's own PCM, `fbb0cb63d6490fea…`.

### 32-bit FLAC

| Decoder | Probe score | Read it | Reported | Bit-exact |
|---|---|---|---|---|
| `decode_flac` | 100 | yes | `48000 Hz / 2 ch / S32` | **yes** |
| `decode_native` | 60 | **no** | — | — |
| `decode_mf` | 40 | **no** | — | — |
| `decode_ffmpeg` | 30 | yes | `48000 Hz / 2 ch / S32` | **yes** |

`decode_native` is the known one: `dr_flac` opens a 32-bit FLAC, reports 32 bits
from STREAMINFO, and decodes nothing — the section above records that at length.

**`decode_mf` is the new one.** Media Foundation refuses a 32-bit FLAC outright,
at 48 kHz, where its documented FLAC limit is a *rate* of about 655 kHz. So the
depth is a second, separate ceiling in the same decoder, and at 32 bits the
reference decoder and FFmpeg are the only two readers in this tree. That is the
strongest case the module architecture has produced: three of the four FLAC
readers here decline the same legal file, each for its own reason.

### 32-bit ALAC

| Decoder | Probe score | Read it | Reported | Bit-exact |
|---|---|---|---|---|
| `decode_alac` | 100 | yes | `48000 Hz / 2 ch / S32` | **yes** |
| `decode_mf` | 100 | yes | `48000 Hz / 2 ch / S32` | **yes** |
| `decode_ffmpeg` | 100 | yes | `48000 Hz / 2 ch / S32` | **yes** |
| `decode_aac` | 100 | no | — | — |

All three ALAC readers are bit-exact to the ceiling of the format, which is what
the ALAC sections above claim for `decode_alac` and is now measured for the other
two as well. `decode_aac` scores 100 because it recognises the M4A container and
then declines the file when it opens it and finds no AAC in it, which is the
resolution rule working: the score is about the container and the refusal is
about the contents.

Note that `decode_mf` reads 32-bit ALAC *in stereo* perfectly. It is multichannel
ALAC it gets wrong, and it declines that rather than getting it wrong now — see
*What Media Foundation does with multichannel ALAC* above.

### Reproducing them

```
flac --totally-silent -f -o c.flac src_s32_48000_2.wav
refalac -s -o c_alac.m4a src_s32_48000_2.wav
mediaperch-probe claims --file c.flac
mediaperch-probe decode --file c.flac --decoder flac
```

`claims` prints every decoder's probe score for one file, which is how the
"probe score" column above was filled and how the refusals were told apart from
the declines.

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
- **Nothing claimed WMA except Media Foundation.** The generated matrix asks every
  decoder about every file, and `decode_ffmpeg` read the WMA when it was forced
  and scored **0** on it when it was asked -- so on a machine with no Media
  Foundation, a WMA was a file nothing would open even with FFmpeg installed.
  ASF's GUID is now in the fallback list at 30, which leaves `decode_mf` winning
  it on Windows at 100 and gives the Linux head an answer. A gap, not a policy,
  and a table nobody generated would not have shown it.
- **Media Foundation refuses 32-bit FLAC**, at a rate it handles happily at 24
  bits. Its documented FLAC limit was a rate; this is a second ceiling, on depth,
  in the same decoder. Measured in *32 bits, measured by hand* above.
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
- ~~Anything above two channels, at any depth.~~ Stale: 5.1 and 7.1 are
  measured above, channel by channel, and the generated matrix carries 5.1 rows
  for WAV, FLAC and ALAC. Left visible rather than deleted because the reason it
  went stale is the point of generating the table.
- DSD in any form: DoP is designed for and not implemented.
- Shorten, TAK and OptimFROG. `decode_ffmpeg` claims them on their documented
  magic bytes and nothing here can encode one, so the claim is unmeasured.
- Monkey's Audio and Musepack, for the same reason: claimed, no encoder to hand.

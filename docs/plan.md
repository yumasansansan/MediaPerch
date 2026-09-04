<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# MediaPerch — implementation plan

A media player that goes straight to WASAPI exclusive when nothing needs to be done to the
samples, and delegates HDR to the operating system instead of reinventing it.

This document is the plan of record. It carries the decisions that are expensive to revisit
— the language split, the module ABI, and the two audio code paths — and the API-level
findings that would otherwise have to be rediscovered by reading Microsoft's documentation
twice.

---

## 1. Goals and constraints

| | |
|---|---|
| Core language | C++23 (C++20 as the guaranteed floor for library features) |
| Module language | **anything that can export a C symbol.** v1 is C and C++ only; the ABI has this shape so that a second language stays a cheap option rather than a rewrite — see §2 |
| Toolchains | MSVC on Windows; Clang, GNU driver, for the fuzzers and for Linux when there is a Linux head. **GCC is not supported anywhere**, and neither is clang-cl — a compiler that accepts MSVC's spellings and means different things by several of them costs a second reading of every flag in the build. Configuration refuses both rather than drifting into either |
| Layer | as low as practical. Prefer the platform API over a wrapper when the wrapper adds no capability we need |
| Audio | WASAPI **exclusive**, event-driven, MMCSS `Pro Audio`. Shared mode is a fallback, not the design centre |
| Bit-exactness | a testable property, not a marketing word. §12 says how it is tested |
| Video | Direct3D 11 + DirectComposition. HDR delegated to **the OS tone mappers** by default — with a correct one selectable, because the OS one is known to be wrong (§9.2) |
| Modularity | decoders, sinks, DSP and the video presenter are runtime-loaded shared libraries behind one C ABI |
| Shell | separate process, optional, replaceable. The engine is complete without it |
| Windows floor | Windows 10 2004 for audio; Windows 11 22H2 for Advanced Color; Windows 11 24H2 for the desktop HDR-state APIs, degrading gracefully below each |
| IDE | Visual Studio 2026, opened as a folder: one CMake generator, Ninja, and `CMakePresets.json` is what the IDE reads |
| Licence | `GPL-3.0-or-later`. Compatible with FFmpeg in either its LGPL or GPL configuration |

Non-goals for v1: macOS, a scripting language, network streaming clients, a library
database with a query language, DLNA, and any form of DRM.

---

## 2. Which language, and where the boundary is

The question is **not** "C++ or Rust". It is *what the boundary between modules is made of*
— and once that answer is "a C ABI across a `.dll` on disk", the language question stops
being architectural and becomes a reversible, per-module choice that can be made later at no
extra cost. That reframing is what decides it below.

### Where C++ wins, and it is not close

Everything MediaPerch touches on Windows is **COM**: `IMMDeviceEnumerator`, `IAudioClient`,
`IAudioRenderClient`, `IAudioClock2`, `IMFSourceReader`, `IMFTransform`,
`ID3D11VideoContext`, `IDXGISwapChain4`, `IDCompositionDevice`, `ID2D1Effect`. In C++ those
are the *native* form of the API — `ComPtr`, `HRESULT`, `__uuidof` — and so is every
sample, every PIX and Media Foundation trace article, and twenty years of answers.

In Rust via [`windows-rs`](https://github.com/microsoft/windows-rs) the same interfaces
exist and are generated automatically — Direct3D 12 has official samples in the repository
— but calling them is `unsafe` end to end. That is the sharp point: **in the layer that is
most Windows-specific, Rust's guarantees are suspended**, and what remains is better enums
and worse documentation. Media Foundation's video plumbing in particular is a place where
you want the search results to be in your language.

Two more reasons specific to this project:

- **The video path is D3D.** `ID3D11VideoContext::VideoProcessorSetOutputColorSpace`,
  `ID3D11VideoContext2::VideoProcessorSetStreamHDRMetaData` and the Direct2D HDR tone map
  effect are C++ APIs with C++ samples, and §9 makes them load-bearing.
- **DragonPerch is C++23** with CMake presets, clang-tidy, Catch2 and libFuzzer. For one
  maintainer with two native Windows/Linux projects, sharing the build system, the CI shape
  and the muscle memory is worth more than it sounds.

### Where Rust would win

- **Parsers.** MediaPerch reads more hostile input than DragonPerch does: ID3v2, APE tags,
  Vorbis comments, embedded cover art, cue sheets, playlists, FLAC frame headers.
  Historically this is where players get CVEs, and Rust removes the class outright.
  [`symphonia`](https://github.com/pdeljanov/symphonia) also ships FLAC/WAV/MP3/AAC/Vorbis
  decoding, so it is not purely a safety argument — it is code not written.
- **Threads.** `Send`/`Sync` checked at compile time is a real guarantee about a lock-free
  ring shared between an MMCSS `Pro Audio` thread and an I/O thread. C++ offers discipline
  and a comment.

### Why v1 is C and C++ only anyway

The Rust case above is real, and it still loses here, for one reason that is easy to miss:

> **The largest parsing attack surface in this program is FFmpeg, and FFmpeg is C no matter
> what language MediaPerch is written in.**

Rust would protect the small surface we write ourselves and none of the large surface we
link. The mitigation that actually covers the risk is **process isolation** — hosting
`decode_ffmpeg` out of process behind the same ABI (§4) — and that mitigation is
language-agnostic, covers our own parsers as well, and is one mechanism instead of two.
Spend the complexity budget there.

The rest follows:

- `decode_native` in C++ is not more work than `decode_native` in Rust. `dr_flac.h`,
  `dr_wav.h` and `dr_mp3.h` are single public-domain headers with no build-system footprint
  at all, and DSF/DFF is a header plus raw blocks. Cargo's convenience argument mostly
  evaporates against three `#include`s.
  **Stale, and it is the bullet that aged worst.** Two of those three headers are gone:
  FLAC went to libFLAC and MPEG audio to libmpg123, both because the reference reader turned
  out to be better than the convenient one. What was left of "the parsers we write" then went
  to Rust, where the *first* bullet's argument -- that this is a small surface -- is what made
  it cheap. `dr_wav.h` is the only one still in the tree.
- The safety gap is closed the way DragonPerch already closes it: libFuzzer on every parser,
  ASan/UBSan in CI, `/GS` and `/guard:cf` in release. That machinery exists and the
  maintainer already runs it.
- For one maintainer, the cost that does not appear in a CI log — switching between two
  languages, two dependency ecosystems, two fuzzing setups — is the one that actually bites.

### The decision

> **C++23 for `src/engine` and `src/player`, the platform heads and every module. A plain C module ABI, kept
> exactly as specified in §4 — not because Rust is coming, but because the ABI is the
> expensive thing to change later and making it C costs nothing now.**

**This is deliberately not a rejection of Rust; it is a deferral that costs nothing to
reverse.** The module boundary is a `.dll` on disk, not a link step: no Corrosion, no
mixed-language linking, no CRT mixing, no change to the C++ build. Adding a Rust module in
two years is exactly the same amount of work as adding one today, so there is no reason to
pay for it today.

### Validating the ABI without adopting a second language

The claim "an ABI that has never been crossed from a second language is an ABI that does not
work yet" stays true, so it gets tested rather than assumed — cheaply:

1. **A plain C probe module, in M2.** Roughly a hundred lines, built by hand, not in CI,
   deleted afterwards. It catches most of what goes wrong: name mangling, an exception
   escaping, a non-POD type in a struct, a default argument, `bool` width, a header that
   only compiles as C++.
2. **A Rust probe, once, on the same day.** Also throwaway. It catches the rest: calling
   convention, `repr(C)` layout, and whether a panic can actually be contained at the
   boundary. Half a day, and then the question is settled either way.

### When to revisit

Named, so that it is a decision rather than a someday:

- a fuzzer finds a memory bug in a parser we wrote that is **not** a one-line fix; or
- the parsing surface grows past tags, cue sheets, playlists and INI — a container demuxer
  of our own, say; or
- the Rust probe in M2 shows the boundary is *pleasant* rather than merely possible.
  **It did** -- see [abi/README.md](../abi/README.md). Layout and calling convention were uneventful and a panic is contained by `catch_unwind` at the boundary. That opens the option; it does not on its own take it, because the argument above still holds: Rust protects the small parsing surface we write and not the large one we link.

**Taken, for `codec_alac` first and then for `demux_adts` and `codec_aac`, and the reason is the one the argument above left open.** The
argument was that Rust protects the small parsing surface we write and not the large one we
link -- and since it was written, the small surface shrank: `demux_flac` moved onto libFLAC,
`demux_mp4` onto Bento4, `demux_mkv` onto libmatroska. What remains written here is four
modules -- `codec_aac`, `codec_alac`, `demux_mpa`, `demux_adts` -- and they are exactly the
residue the argument said Rust would protect. ALAC went first because it is integer
arithmetic, so bit-exactness against the recorded hashes is a machine check rather than a
judgement; it passed on every one. `docs/formats.md` has the measurements and
`modules/shared/mp-abi` has the boundary. The cost the deferral predicted -- no Corrosion, no
mixed-language linking, no CRT mixing, no change to the C++ build -- was the cost paid:
`cmake/Rust.cmake` is a `cargo build` and a copy.

`demux_adts` and `codec_aac` followed. The codec was the one this order put last, because
it is float and its recorded hashes are its own, so a port had to reproduce its arithmetic
order or the record would change; it reproduced it, on every hash and every SNR figure, and
came out faster than the C++ once the transform's table index was masked rather than
bounds-checked. `demux_mpa` did not go to Rust at all: it went to libmpg123, which is the
other outcome this paragraph allowed for -- a demuxer becomes a wrapper when a better reader
turns up, the decision this tree made for FLAC, MP4 and Matroska. So the four modules that
were the residue are three Rust crates and one wrapper, and no parser this tree wrote is C++
any more.

### What the shell is written in is a separate question

The shell is a separate process behind an IPC boundary, so its language constrains nothing.
WinUI 3's supported markup languages are C# and C++/WinRT; Rust can drive the Windows App
SDK through `windows-rs` but has no XAML markup compiler, so a Rust shell means building
the UI tree in code. DragonPerch's shell is C# with Native AOT and that pattern transfers
directly: **C#, WinUI 3, Native AOT, and it stays optional.**

---

## 3. Layering

Four rings, and dependencies only ever point inwards.

```
   ┌─ shell (any process, any language, optional) ────────────────┐
   │  ┌─ platform head (src/win, src/linux) ───────────────────┐  │
   │  │  ┌─ modules (C ABI, LoadLibrary/dlopen) ───────────┐   │  │
   │  │  │  ┌─ core (portable, no OS headers) ─────────┐   │   │  │
   │  │  │  │  graph · negotiation · ring · clock      │   │   │  │
   │  │  │  │  playlist · registry · config schema     │   │   │  │
   │  │  │  └──────────────────────────────────────────┘   │   │  │
   │  │  └─────────────────────────────────────────────────┘   │  │
   │  └────────────────────────────────────────────────────────┘  │
   └──────────────────────────────────────────────────────────────┘
```

- **core** knows nothing about `HWND`, `IMMDevice`, files or sockets. It is handed
  interfaces and drives them. CI builds it as a standalone target with the platform
  directories removed from the include path, exactly as DragonPerch does, so the rule is
  enforced rather than intended.
- **the head** owns the message pump, the COM apartment (MTA for the engine, one
  `MFStartup`), the device notification client, the module loader, the config file and the
  IPC server. It is the only code allowed to include `<windows.h>`.
- **modules** never call the head directly. They receive a host vtable at init.
- **the shell** never sees a module. It sees playback state and a settings tree.

---

## 4. The module ABI

One exported symbol per shared library:

```c
/* include/mediaperch/module.h — pure C, no dependencies */
#define MP_ABI_VERSION 1u

const MpModuleDesc *mp_module_entry(uint32_t host_abi_version);
```

Every rule below exists because of the render-thread constraint in
[design.md](design.md).

1. **Pure C, `extern "C"`, no C++ or Rust types, no allocation across the boundary.** The
   caller supplies buffers and the callee fills them. A single host allocator vtable is
   passed at init for the cases that genuinely need one, and it is never called from an RT
   entry point.
2. **Every interface struct begins with `uint32_t size`.** New fields append; a host
   reading an older module clamps at `size`. This is the only versioning scheme that
   survives a third-party module compiled a year ago. `uint32_t` rather than `size_t`
   because it keeps one arch-dependent field width out of every struct in the ABI, and no
   descriptor is going to approach 4 GB.
3. **Nothing unwinds.** C++ entry points are `noexcept` shims; Rust entry points wrap their
   bodies in `catch_unwind` and return an error code. A module that unwinds anyway is
   terminated with a diagnostic, not "handled".
4. **Every entry point carries its thread class** in the header, and the tag is part of the
   contract:

   | Tag | May block | May allocate | Called from |
   |---|---|---|---|
   | `MP_RT` | no | no | the MMCSS render thread |
   | `MP_IO` | yes | yes | the decode/IO thread |
   | `MP_ANY` | yes | yes | control, at graph rebuild |

5. **Unload requires quiescence.** `mp_module_unload` is legal only when the refcount is
   zero *and* the engine has passed a barrier proving no RT thread can be inside module
   code — in practice, at graph rebuild points only. A module that starts its own threads or
   registers COM classes sets `MP_MODULE_NO_UNLOAD` and is honestly leaked for the process
   lifetime instead of being dishonestly `FreeLibrary`-ed.
6. **Capability declaration is data, not code.** The descriptor lists what the module claims
   — kinds, container and codec IDs, sample formats, a priority — so the registry can build
   the resolution table in §7 without loading and initialising every module at every start.

Interfaces in v1:

| Kind | Interface | Notes |
|---|---|---|
| `MP_KIND_DECODER` | `probe`, `open`, `read_packet` (`MP_IO`), `seek`, `close` | reports the source format; never converts |
| `MP_KIND_SINK` | `enumerate`, `negotiate` (`MP_ANY`), `start`, `render` (`MP_RT`), `stop` | `negotiate` is the whole of §6 |
| `MP_KIND_DSP` | `configure`, `process`, `flush`, `set`, `describe` | deinterleaved **f64** in and out (see below); `configure` answers with the format the stage produces, so a resampler can exist |
| `MP_KIND_VIDEO` | `create_surface`, `present`, `set_colour_target` | §9 |
| `MP_KIND_META` | `read_tags`, `read_art` (`MP_IO`) | the most hostile input in the program; fuzzed hardest |


### ABI v2: the container decides

**v1 asks every decoder "can you read this file?" and tries the ones that say yes, in
order.** That is the wrong question, and §4.6 above already knew it: it says the descriptor
should list "kinds, container and codec IDs" so the registry can build a resolution table
without asking anybody. v1 shipped without those lists, and the question became "try them
and see" instead.

What that costs is now measured rather than argued. An MP4 is claimed at 100 by
`decode_alac`, `decode_aac` and `decode_ffmpeg` alike, because the box that names the codec
is in `moov` and `moov` may be at the end of a file a probe sees four kilobytes of --
measured: FFmpeg puts it 1,110 bytes from the end of a 49 KB file, and a two-hour album
puts a megabyte of it there. So the order of attempts owes nothing to what is inside, and
each wrong guess is a module opening a file to say no. The outcome is correct and the
structure is not, and [formats.md](formats.md) records two more of its failures: a probe
that claimed AMR, DTS and FLV by finding a false frame sync, and containers that no module
claimed at all.

**And it gets worse with video and subtitles, which is the actual argument.** A file is not
one stream. "Which decoder reads this file" has no answer for a Matroska with video, two
audio tracks and three subtitle tracks; the question is "what is in it, and which decoder
takes each one". A structure built on trying decoders in order cannot be extended to that
-- it has to be replaced by one built on asking the container.

So v2 is the shape every media stack that survived contact with containers has:

```
identify the container  →  read it  →  it names the codec of each stream  →  the codec is
                                                                            looked up
```

Nothing is tried. The container is identified from its first bytes, which is what magic
bytes are *for* and what the probes here already do well; everything after that is a table
lookup on data the container stated.

#### Three kinds where there was one

| Kind | What it is | Interface |
|---|---|---|
| `MP_KIND_DEMUX` | a container reader | `probe`, `open`, `stream_count`, `stream_info`, `select`, `read_packet`, `seek`, `close` |
| `MP_KIND_CODEC` | a packet decoder | `open(codec_id, config, config_bytes)`, `get_format`, `decode`, `flush`, `reset`, `close` |
| `MP_KIND_DECODER` | **gone.** Its two halves are the two above | — |

`MpStreamInfo` is what a container says about one stream: its kind (audio, video, subtitle),
its codec id, the codec's configuration blob verbatim — `ALACSpecificConfig`,
`AudioSpecificConfig`, a FLAC `STREAMINFO` — the format where the container states one, the
duration, and **the gapless edit**, because that lives in the container and always did:
`elst` in MP4, the LAME tag in an MP3's first frame, `pre_skip` in an Opus header.

The edit's tail needs two fields rather than one, and Matroska is why. `play_frames` is how
long the audio is; `trim_frames` is how many frames at the end are padding. A container
states one or the other and neither converts into the other without the decoded length --
which Matroska cannot give, because every timestamp in it is scaled to the millisecond and a
rounded length applied to a lossless track truncates it. `docs/formats.md` has the four
candidate formulas and the one that lands on the frame.

A codec module never sees a file. It is handed an id, a configuration blob and packets, and
it produces the file's own samples — which is the point worth saying plainly: **the split
adds no conversion.** Bit-exactness is a property of the codec and the container has none of
it to lose.

#### The library that does both

libFLAC reads the FLAC container *and* decodes FLAC. `opusfile` reads Ogg and drives libopus.
Media Foundation and FFmpeg are whole pipelines with a file at one end and PCM at the other.
A split that could not accommodate those would be a split that could not use a reference
implementation, which is not a trade this project makes.

So a stream may be flagged `MP_STREAM_SELF_DECODES`: the demuxer that produced it will also
decode it, and the host asks the same module rather than looking up a codec. That is one
structure with one declared exception, not two structures. It is also honest about what such
a module is — Media Foundation is not a container reader that happens to decode, it is a
pipeline, and the flag says so.

Where the split *is* available it is taken, because Xiph already ships the pieces separately
and this tree already vendors them separately: `external/ogg` is the container, `vorbis` and
`opus` are the codecs. Ogg is where v2 costs least and shows most.

#### What each module becomes

| Today | Becomes | Note |
|---|---|---|
| `modules/shared/mp4/mp4.cpp` | `demux_mp4` | **already a container parser**, already shared by two modules. It gains a vtable and stops being a library. *(Later replaced outright: `demux_mp4` reads MP4 with Bento4 now, and this file is gone. See docs/formats.md.)* |
| `decode_alac/alac.cpp` | `codec_alac` | **already a pure codec**: it takes a config blob and packets |
| `decode_aac/aac.cpp` | `codec_aac` | likewise. Both keep their fuzz targets unchanged |
| `decode_aac`'s ADTS framer | `demux_adts` | **the row this table was missing.** `decode_aac` had three parts, not two: a codec, half an MP4 parser, and a framer for raw AAC. Leaving the third out would have taken `.aac` from a first-class format to an FFmpeg-only one |
| `decode_mp3` | `demux_mpa` + `codec_mpa` | the "container" is frame headers and the LAME tag, which is a demuxer's work; `dr_mp3` decodes |
| `decode_native` | `demux_wav` + `codec_pcm` | `dr_wav` reads the container; PCM's codec is a memcpy, which is the honest description of what Path A already does. **`codec_pcm` has no dependency at all**, so every container that carries uncompressed audio -- MP4, Matroska, CAF, the day they name it -- gets a decoder for free |
| `decode_native`'s FLAC half | *dropped* | it was `demux_flac` + a dr_flac codec, and the second was written and then removed: `demux_flac` needs no library either, so the pair added a reimplementation with a known 32-bit hole and nothing else. FLAC without libFLAC now falls to FFmpeg like any other unimplemented codec |
| `decode_flac` | `demux_flac` (written here) + `codec_flac` (libFLAC) | the container is ours because a FLAC frame carries no length and finding its end is a scan the reference decoder does not expose; the codec is libFLAC driven one frame at a time, opened on the STREAMINFO the container hands over. **OggFLAC starts playing as a consequence**, through `demux_ogg` and the same codec |
| `decode_ogg` | `demux_ogg` (libogg) + `codec_vorbis` + `codec_opus` | the case that pays for itself: OggFLAC and Speex stop being "an Ogg this module cannot read" and become an Ogg whose codec nobody has yet |
| `decode_mf` | `demux_mf`, every stream `SELF_DECODES` | one stream, deliberately. A source reader can enumerate them, but this is the floor and making the least trustworthy path more capable is the wrong direction |
| `decode_ffmpeg` | `demux_ffmpeg`, every stream `SELF_DECODES` | `ffprobe` already enumerated streams and the module threw that away and asked for `a:0`. It reports all of them now, and `select` picks -- so a film with a commentary track has two tracks rather than one |

Two things fall out of the table that are worth naming. **The MP4 problem disappears
entirely**: `demux_mp4` reads `moov` wherever it is, in `open`, where reading a file is
allowed — there is no four-kilobyte window because a demuxer is not a probe. And
**`decode_ffmpeg` stops being a fallback for containers it can read and becomes a demuxer
for them**, which is a better description of what it is.

#### What this costs, and what it breaks

- **`MP_ABI_VERSION` goes to 2**, and every module in the tree is rewritten. Nothing has
  shipped, so nothing outside this repository breaks. The C and Rust probes in `abi/` are
  rewritten with it, which is the second thing they are for.
- **Seeking splits in two**, and the seam is real: the demuxer seeks to a packet boundary
  and the codec has to be reset and given its pre-roll — AAC needs a priming frame, MP3 has
  a bit reservoir. v1 hid that inside each decoder; v2 makes it the host's, once, instead of
  each module's, repeatedly. That is the change with the most room to be got wrong and it
  gets the seek test that already exists, per codec.
- **The registry's resolution table is rewritten** (§7): container by probe score, codec by
  id. `mediaperch-probe claims` becomes a report about both.
- **The gapless edit moves** from the decoder to `MpStreamInfo`, which is where it belongs
  and where a second consumer — a tagger, a video path — can see it.

#### The order to do it in

Each step leaves the tree building and playing music.

1. **Done.** The ABI header: the two vtables, `MpStreamInfo`, the codec ids, and the
   descriptor's capability lists that §4.6 asked for. Nothing implements them yet.
2. **Done.** The host: resolve by container then codec, with `MP_KIND_DECODER` still
   supported, so the old and new modules coexist while the rest of the list is worked
   through. `mp::PacketSource` is where a demuxer and a codec become one `ISource`, and it
   is the single place the gapless edit, the seek warm-up and the packet buffer live.
3. **Done.** `demux_mp4` + `codec_alac` + `codec_aac` — the split that already exists in the
   tree, so it is the one that proves the shape with the least new code. Every MP4 in the
   corpus decodes to the same hash through the pair as through the decoder it replaces.
4. **Done.** `demux_ogg` + `codec_vorbis` + `codec_opus`, which is where the split buys a
   format (OggFLAC, Speex) rather than only tidiness. Byte-identical to `decode_ogg` on both
   codecs, and OggFLAC and Speex now get read by `demux_ogg` and refused by name — "nothing
   here decodes that codec" instead of "this is an Ogg I cannot read", which is the
   difference the split was for.
5. **Done, and it needed a fourth demuxer the table above missed.** `demux_wav`
   + `codec_pcm`, `demux_flac` + `codec_flac`, `demux_mpa` + `codec_mpa` -- and
   `demux_adts`, because `decode_aac` had three parts rather than two and the
   ADTS framer was one of them. Without it a raw `.aac` would have gone from a
   format with a first-class reader to one only FFmpeg could open, which is
   exactly what "each step leaves the tree playing music" forbids.
6. **Done.** `demux_mf` and `demux_ffmpeg` with `SELF_DECODES`, converted in
   place rather than added beside the decoders they replace -- so what is left
   for step 7 is deletion and nothing else. `demux_ffmpeg` enumerates every
   audio stream, which is the capability the v2 shape bought here: the module
   used to ask `ffprobe` for `a:0` and throw the rest away, so a film with a
   commentary track had one track as far as this program was concerned.
   `mediaperch-probe`'s `compare`, `verify` and `loudness` went across with
   them, because the module they measured against was one of the two.
7. **Done.** `MP_KIND_DECODER` is deleted, and with it the last of "try them in
   order". `grep MP_KIND_DECODER` finds two comments saying it is gone and
   nothing else: no vtable, no kind, no `mp::Decoder`, no `decoders_for`, and no
   `modules/decode/`. The kind's number is left as a hole rather than reused --
   an id that meant something else once is an id a host can get wrong.

**Every demuxer is a parser that reads a file somebody else wrote, so every
demuxer wants a fuzzer.** Where the parsing is somebody else's it is fuzzed
somewhere else -- dr_wav and dr_mp3 by their own targets here, libogg by Xiph
upstream -- and where it is ours it is fuzzed here: FLAC's framing through
`flacframe_fuzzer`, added with step 5.

**Somebody else's parser gets one too when this tree is the one that adopted
it.** `demux_mp4` was rewritten on Bento4, which deleted `shared/mp4/mp4.cpp`
and the fuzz target that linked it; `mp4_fuzzer` replaced both, because trading
five hundred fuzzed lines for eighty thousand unfuzzed ones is not a trade worth
making. It found **two** denials of service in Bento4 in its first ten minutes,
both the same defect -- a box states an entry count, the parser loops that many
times, and nothing checks the count against the bytes the box has. `sgpd`: 2 GB
and no return, from a 1143-byte file. `dref`: 84 seconds, from a 1269-byte one.
That is the argument for the target in two sentences. `docs/formats.md` has the
mechanism, the two defences, and the upstream fix.

That one earned its keep in ninety seconds. FLAC frames carry no length, so
`demux_flac` finds the end of one by running the format's CRC-16 forward and
testing every position where it reads zero -- and the scan started by reading
the shortest frame the header implied, which can be more bytes than it was
given. Eighteen bytes beginning `FF FB`, an MPEG sync that also satisfies FLAC's
fourteen-bit one, read past the end of the buffer. Reachable for real on a file
whose last frame is truncated. The input is in `fuzz/corpus/flac`.

Still not fuzzed and worth saying so: `demux_mpa`'s LAME-tag reader and
`demux_adts`'s header parser, both of which are ours and both of which read
attacker-controlled bytes. They want the same treatment.

What they have instead, for now, is a sweep: every file in the corpus truncated
at seven fractions and bit-flipped three ways, put through `decode` and `claims`
against a build with AddressSanitizer on. 360 invocations, no crash and no
report. That is weaker than fuzzing -- it explores what a real file looks like
when it is damaged rather than what an attacker would write -- and it is what
was run.

**And a second compiler front end, which was the hole the deletion exposed.**
Both Clang presets in this tree set `MEDIAPERCH_BUILD_PLATFORM=OFF`, because the
Windows head does not build with a GNU-driver Clang -- so the modules, which are
most of the code, had only ever been parsed by MSVC. That is not a small gap:
MSVC types an unscoped enum as `int` regardless of its values, so
`MP_CODEC_INTERNAL = 0xFFFFFFFF` was quietly -1 and nothing said so until a file
using it happened to be compiled into a fuzzer. `ctest -R clang_syntax` now runs
`clang++ -fsyntax-only` with the project's warning set over every portable
source; it takes about a second, links nothing, and found three modules keeping a
host pointer they never read and two loops mixing signed and unsigned in a tree
that compiles with `-Wconversion` on purpose. See `cmake/ClangSyntax.cmake`.

**Step 7 was not optional and was not "later".** A migration that leaves both structures in
the tree has not replaced anything: it has added a second way to do the same thing, and the
first one goes on working, so nothing forces the last module across. The interface was marked
*being removed* in the header from the day step 1 landed, and the milestone was not done
until `grep MP_KIND_DECODER` found nothing.

Three things came out of the deletion that were not the deletion, and each of them is what
a second structure had been hiding:

- **A submodule left the tree.** `opusfile` is the container and the codec in one object,
  which is what `decode_ogg` used; `codec_opus` drives libopus directly. With `decode_ogg`
  gone there was no caller, so `external/opusfile` went too, along with the two CMake shims
  that existed only to make its version discovery work and the `OP_DISABLE_HTTP` setting
  somebody had to remember. A decoder that can open a socket is a decoder with an attack
  surface it did not need, and the surest way not to have one is not to build it.
- **Each remaining submodule sits with the one module that needs it.** libFLAC is a *codec*
  dependency and nothing else, because `demux_flac` reads the container with no library at
  all; libogg belongs to `demux_ogg` and libvorbis and libopus to their codecs. One module
  used to bring in four of them, which is what "the container and both codecs at once"
  looks like in a build file.
- **A file only one compiler had ever read is now read by two.** See below.

Two things fall out of the order that are worth stating now, because they are what the
result has to look like:

- **`demux_ffmpeg` is the fallback and `demux_mf` is the floor.** FFmpeg claims a container
  it can read at the fallback score; Media Foundation claims the same container below it, so
  it is chosen only where nothing else is. That is the ordering §7 already measures its way
  to, expressed as data instead of as a probe's opinion.
- **Both are `SELF_DECODES`, and both are honest about it.** Neither is a container reader
  that happens to decode; each is a pipeline, and the flag says so rather than pretending
  they are demuxers that could be paired with somebody else's codec.


### Whether this ABI could carry a DAW's engine

It is intended to, so the question is asked here rather than discovered later. The short
answer is yes, and the reason is not that the ABI is complete — it is that **the two things
that cannot be added later are already right**, and everything found missing can be appended
without breaking a module compiled today.

**What is already right, and it is the expensive half:**

- **Thread classes are part of the contract, not a convention.** `MP_RT` may not block,
  allocate or unwind, and it says so on every entry point. Most plugin APIs leave this to a
  guideline and a hope; an engine that must not miss a 3 ms deadline cannot.
- **Everything is size-prefixed and only ever grows at the end**, so a field added in two
  years costs a module nothing. This is the whole reason the answer above is "yes": what is
  missing is missing, not precluded.
- `configure` / `process` / `flush` / `reset` is already block processing with a variable
  output count — which is what lets a resampler be a stage at all, and what a bounce at a
  different rate needs.
- Deinterleaved `f64` planes, which is what a mixing engine wants and what a player only
  needs because Path B exists.
- The container/codec split of v2 *is* project import: a DAW opening a file asks the same
  two questions in the same order.

**What was missing and could not wait: latency.** Three stages here have it and all three
reported it only through `describe`, as a sentence. Nothing could ask. For one chain that is
a cosmetic gap — the position a player reports is the device's, and with a linear-phase
stage the audible audio is that far behind it. For an engine summing several chains into one
bus it is fatal: the short chains must be delayed to match the long one, and getting that
wrong moves tracks against each other, which is the one error in a mixer that is never
subtle. `MpDspVtbl::get_latency` was added for this, at the end of the vtable where growth
is allowed, and a 1023-tap linear-phase equaliser now reports 511 frames to anything that
asks.

**What is missing and can wait, because appending is cheap:**

| Missing | Why a DAW needs it | Where it would go |
|---|---|---|
| Numeric, automatable parameters | `set(key, value)` is strings on the control thread. Automation needs an id, a range, and a change that lands on a known sample with a ramp | `param_count`, `param_info`, `set_param(id, value, frame_offset)` appended to `MpDspVtbl` |
| Encoders and muxers | a DAW bounces, and nothing in this tree writes a file | `MP_KIND_ENCODER` and `MP_KIND_MUX`, the mirror of what v2 just built for reading |
| Several buses, and sidechains | `configure` is one format in and one out | a bus index on `configure` and `process`, appended |
| Events | instruments, if the engine ever hosts one | a separate vtable; not an audio question |

Two things are deliberately *not* on that list. **A timeline with a tempo map is not the
module ABI's business** — a player's transport is a file position and a DAW's is a musical
one, and both live above the boundary in exactly the same place. And **state save and
restore already works**: `describe` reports every setting and `set` takes it back, which is
what the settings file round-trips through today and what a project file would.

### ABI v3: what appending cannot reach

Asked when the video work was about to start, and answered before it, because the answer
decides whether M6's first commit is a header or a renderer. **Almost everything appends.
Two things in the demuxer do not, and one thing is not a widening but a new kind.**

**Done, except the new kind.** `MP_ABI_VERSION` is 3: `select` became `select_streams`,
`seek` names its stream, `MpPacket::reserved` became `stream`, and `stream_video_info` was
appended after `close`. `MP_KIND_VCODEC` is *not* in -- a kind number with no vtable and no
module is the mistake this tree already made once with `MP_ENCODING_DSD` and reverted, so it
lands with the decoder that implements it.

| Need | Appendable? |
|---|---|
| Video geometry, and the primaries/transfer/matrix §9 turns on | **yes** — a `stream_video_info` call, appended. `MpStreamInfo::format` stays what it says it is |
| Presentation | **yes** — `MP_KIND_VIDEO` is already reserved as 4, and a vtable behind a reserved number costs nothing |
| A DSP error string | **yes**, and it did: `describe`'s `trouble` key, without touching the vtable |
| Numeric automatable parameters, encoders, buses, events | **yes**, all four, as the table above says |
| **Several streams from one file** | **no.** `select` names one stream and `read_packet` reads it. Appending `select_streams` leaves `select` meaning something narrower than its name, and `MpPacket` has to say which stream a packet came from — `reserved` is sitting there for it, but a field that changes meaning is a break whatever it is called |
| **Seeking, once there are several** | **no.** `seek(frame)` is "the selected stream". With two selected there is no answer, and an appended `seek_stream(index, frame)` leaves two functions where the older one is now a trap |
| **Video decode** | **not a widening.** `MpCodecVtbl::decode` writes PCM into the caller's buffer. A hardware video decoder produces a texture it owns, in a pool; copying a 4K NV12 frame out at 60 fps is 750 MB/s spent to undo the reason for decoding on the GPU. A second `decode_frame` in the same vtable would make a codec module implement one of two output models, which is the shape with two meanings §15 warns about. It is a **new kind**, `MP_KIND_VCODEC`, beside the reserved `MP_KIND_VIDEO` |

So the break is narrow: `MpDemuxVtbl::select` and `seek`, and `MpPacket::reserved`. Nothing
in `MpCodecVtbl`, `MpSinkVtbl` or `MpDspVtbl` moves, and no audio module changed except to be
recompiled.

The cost of breaking is worth stating because it is nearly nothing and will not stay that
way: `mp_module_entry` refuses a mismatched `MP_ABI_VERSION`, so a bump makes every module
fail to load until it is rebuilt — and **every module in the world is in this repository.**
The same was true at v1 to v2, which deleted a whole kind. It was true at v3. It stops being
true the first time somebody else ships one.

#### What v3 looks like

`select_streams(indices, count)` replaces `select(index)`. Selecting again replaces the set
rather than adding to it, and `count` is at least one. **A demuxer may decline `count > 1`
with `MP_ERR_UNSUPPORTED`** — most containers here hold one stream and have nothing to
interleave, and declining is a real answer rather than a failure to implement.

The empty set nearly meant something. It was written into the header as "how a host stops
reading", implemented in `demux_mp4`, and not implemented in the five single-stream demuxers,
where selecting nothing left the reader where it was and `read_packet` went on returning
packets — five modules disagreeing with the header, which is the worst kind of
documentation. The fix was not to implement it five times: **nothing in this tree wants it.**
A host that has stopped reading closes the demuxer, and a player turning off a video track
selects the audio stream rather than none. §15's rule about not adding an interface before
its second implementation applies to a *meaning* as much as to a function, and loosening
`count == 0` later is not a break.

`read_packet` returns the next packet of *any* selected stream **in the order the container
stores them**, and `MpPacket::stream` says which. Storage order and not a schedule: a host
that wants audio and is handed video keeps the video packet. That queue is the host's to
own, and it is the only arrangement that reads a file once.

`seek(stream, frame)` names the stream, and `frame` is in that stream's own rate. `stream`
need not be selected — a host seeking by the audio clock names the audio stream whether or
not it is reading it. **One file has one position**, so the seek moves every selected stream:
each arrives from wherever its own nearest sync point was, and the host discards what
precedes its own target, per stream, because the points are not the same point.

`stream_video_info(index, MpVideoInfo*)` is appended after `close`. Geometry, the display
size when the pixels are not square, the frame rate as a ratio — and the three code points
§9.1 turns on, which is the reason it exists: nothing but the container says whether the
frames are BT.709 or BT.2020, or whether the transfer is sRGB or PQ, and a renderer that
guesses produces a picture that is merely plausible. Mastering-display metadata is not there
yet; it appends the day §9.3's `driver` provider is written, because a field nothing fills is
a field nothing checks.

#### What it was checked against

Every other test in this tree drives fakes, deliberately: a fake says exactly what the host
is being tested against. `tests/demux_v3_test.cpp` does not, because what is under test is
whether the *shape* works on a container somebody else's tool wrote — and a fake that
interleaved the way I imagined MP4 interleaves would prove nothing at all. It loads
`mp_demux_mp4.dll` through `mp_module_entry`, which is also where the version bump is
visible, and reads a 14 KB MP4 with one H.264 track and one AAC track:

- both selected, the packets come back interleaved, each saying which stream it is, and the
  counts are the 45 and 24 that ffprobe reports;
- one selected, only that stream's 45 arrive;
- an empty set is refused, because it never had a caller;
- a stream that is not there, and one named twice, are both refused;
- a seek to audio frame 22050 moves both streams and lands at or before the target;
- the frame rate comes back as 24000/1001 rather than as a rounded 23.976;
- and the colour code points come back as the container states them -- 2/2/2 in one file,
  and 9/16/9 with full range in a second that is **the same file with four bytes changed**,
  so a difference is the `colr` box being read and nothing else.

That last file is made by hand, by `tools/make_av_fixture.py`, because this FFmpeg build
would not write the code points it was asked for: `-color_primaries bt2020 -color_trc
smpte2084` and the equivalent x265 parameters both produced a `colr` box reading 2/2/2. The
fixtures are committed rather than generated at test time, because multi-stream demuxing is a
correctness property of the ABI rather than a quality measurement and belongs in the default
test run, which must not depend on FFmpeg being on the machine.

#### Two containers, which is what makes it an interface

`demux_mkv` serves several tracks too, and doing it there is what turned v3 from one module's
habit into a shape. MP4 stores samples in a flat table and Bento4's `AP4_LinearReader`
already knew how to walk several tracks in storage order; Matroska stores blocks inside
clusters, laces several frames into a block, and this module's reader had a lace cursor and
an idea of the frame rate that were both **the one selected track's**. What changed is
`selected` becoming a set, `frames_of` taking the track whose rate makes its answer mean
anything, and `next_block` accepting any selected track and recording which one it found. A
scoped `OnlyTrack` narrows the selection for the two open-time helpers that walk the file
looking for one track's blocks.

One thing it found: **a video packet was claiming a position it did not have.** `frames_of`
answers 0 for a track with no sample rate, and the packet was flagged `MP_PACKET_TIMED`
anyway -- which says "position 0, and I mean it" for every frame of the video, when the flag
exists precisely to distinguish that from "I do not timestamp". It is cleared now for a track
that has no frames to count.

`demux_ffmpeg` still declines, for a different and permanent reason: it is a child process
decoding to a pipe, so a second stream would be a second `ffmpeg`. `demux_mf` declines
because an `IMFSourceReader` is a pipeline with no seam in it, which is §9.8.

**Out-of-process modules use the same ABI.** A future `mp_host_ffmpeg.exe` implements the
identical vtable over shared memory and a pipe, so a decoder that dies on a malformed file
takes a helper process down and not the audio. The ABI is shaped now so that this needs no
redesign later; it is not built in v1.

---

## 5. The audio engine

Two graphs, chosen once when a track starts, never branched between at run time.

### Path A — passthrough (the default)

```
  decoder ──packets already in the device's format──▶ SPSC ring (bytes) ──▶ GetBuffer / memcpy / ReleaseBuffer
```

No float. No gain. No mixing. No sample-rate conversion. No dither. The ring holds device
bytes and the render callback performs one `memcpy`. Volume, if the user wants any, is
`IAudioEndpointVolume`: the session interfaces (`ISimpleAudioVolume`, `IAudioStreamVolume`)
have **no effect at all** on an exclusive-mode stream, which is a fact worth a comment
beside the volume control.

Three variants live here, and all three are `memcpy`:

- **PCM.** Sample rate, bit depth and channel count all survived §6 unchanged.
- **DoP.** DSD wrapped in 24-bit PCM frames with the alternating `0x05`/`0xFA` marker,
  offered to the device as 176.4 or 352.8 kHz 24-bit. The device never learns it is DSD.
- **IEC 61937 bitstream.** Dolby Digital / DTS / E-AC-3 handed through as a
  `WAVEFORMATEXTENSIBLE` with a `KSDATAFORMAT_SUBTYPE_IEC61937_*` subformat, for a receiver
  to decode.

### Path B — processed

```
  decoder ─▶ f64 deinterleaved bus ─▶ DSP chain ─▶ requantise + dither ─▶ ring ─▶ sink
```

Entered when the user asks for DSP, when a resample is unavoidable, or when negotiation
failed and the user chose to convert rather than not play. The canonical bus is
deinterleaved because every DSP anyone will write wants it that way, and one conversion at
each end is cheaper than N conversions inside.

**The bus is f64, not the f32 this plan first said.** The conversion at each end already
works in binary64, and an f32 bus would add a second rounding to the one path whose argument
is that it has exactly one; the cost is memory bandwidth on a workload measured in tens of
megabytes a second. design.md §"The DSP chain" carries the reasoning.

**What is written so far** is `src/engine/processed.*`, `src/engine/processor.*`,
`src/engine/convert.*`, `src/engine/dither.*`, `src/engine/shaper_tables.*` and
`src/engine/dsp.*`: the graph, the
sample-type conversion through a normalised `double`, five dither distributions, binomial
noise shaping of any order to 9, 79 transcribed shaping curves, a gain, and the chain
itself. `modules/dsp/gain` is the first stage behind `MpDspVtbl`, and exists as much to
prove the ABI carries a stage as to turn anything down.

A stream with no stages still goes straight from the source to the wire format in one
conversion: the bus is built only when there is a chain to put on it.

`modules/dsp/resample` is the second stage and the one that proves the shape: it answers
`configure` with a sample rate it was not given, and the chain's output format, what
negotiation offers the device and how much room the graph allocates all follow from that
answer. It is asked for and never inserted automatically -- §6's refusal is the point of
this program, and a resampler that appears whenever a device is fussy would quietly end it.

`modules/dsp/mix` is the third and last geometry: the channel matrix. With the sample type,
the rate and the channel count all reachable, **the only thing that can still make a device
refuse a file is a device that refuses everything** -- and every one of them is asked for
rather than inserted, which is what keeps §6's refusal meaning something.

**Path B can be hashed without a device, and until recently could not.** The arithmetic
lived inside `ProcessedGraph`, which needs a sound card, a ring and two threads, so
`mediaperch-probe decode` took `--path processed`, `--gain` and `--dsp`, printed them back,
and ignored them -- a -6 dB gain and a resample to 192 kHz both left the SHA-256 where it
started. Every claim in this document about the resampler, the dither and the shapers rested
on listening to them. `mp::Processor` is that arithmetic with the device, the ring and the
threads taken out; `ProcessedGraph` holds one and does nothing else with the samples, so
what `decode` measures is the play path rather than a second copy of it. What it found on
the first run, and what the two instruction-set baselines turned out to agree on, are in
[formats.md](formats.md) under *Path B, hashed*.

`modules/dsp/vst3` is the stage that is not like the others: it loads a DLL somebody else
wrote. **It changes what this project can promise** -- a chain with a VST3 in it is exactly
as reproducible as the plugin is -- which is why it is a module a person asks for by name
and never something that appears on its own, the same rule §6 applies to the resampler. It
became possible in October 2025, when Steinberg relicensed the VST3 SDK to MIT; before that
the licence was incompatible with GPLv3 in both directions. It takes `pluginterfaces` and
writes the host, which is eight hundred lines against `public.sdk`'s forty thousand, and it
hands a plugin this tree's f64 bus untouched when the plugin says it can take doubles.

### Switching between them

At a **graph rebuild point** only: track change, device change, format change, or an
explicit user toggle. A rebuild stops the client, tears the graph down, renegotiates and
starts again — an audible gap of a few tens of milliseconds. That gap is the correct trade.
A hot swap means the render callback contains a branch on state another thread writes, and
that is the bug that costs a week.

### Threads

| Thread | Priority | Does |
|---|---|---|
| render | MMCSS `Pro Audio` | wait on the event, `GetBuffer`, fill, `ReleaseBuffer`. Nothing else, ever |
| decode | normal | read ahead into the ring, stay some hundreds of ms in front |
| control | normal | IPC, module load/unload, graph rebuild, device notifications |

The ring is single-producer/single-consumer, power-of-two, acquire/release indices, no CAS.
It lives in `src/engine` and is one of the two things `tests/` cares most about.

### Volume, and why Path A has none

A software volume control is a multiply. A multiply on integer samples either rounds or goes
through float, and both are conversions — so **a volume slider is a Path B feature, and
putting one in Path A would quietly make Path A into Path B for everybody who touched it.**

The only volume that costs nothing is one applied below us, and `IAudioEndpointVolume` is
how you reach it. But its capability query says less than its name suggests:
`QueryHardwareSupport` returning `ENDPOINT_HARDWARE_SUPPORT_VOLUME` means the control is not
implemented by the Windows audio engine. It does **not** say whether the driver applies it by
scaling samples — which costs bits like any other gain — or whether the hardware applies it
after the converter, which costs none. Measured on this machine: all four endpoints claim it,
including a virtual cable that has no hardware at all.

So the design is:

| Device | What the UI offers |
|---|---|
| claims `ENDPOINT_HARDWARE_SUPPORT_VOLUME` | the endpoint's own volume, labelled as the device's, with the caveat that whether it is free is between the user and their DAC |
| does not | **no volume control at all**, and a sentence saying why |
| either, with Path B chosen | a normal software volume, and the UI says the stream is being processed |

Showing no control is the honest option and the one to default to. A player that grows a
slider which silently reroutes the stream has given up the only property it was built for,
and the person using it will not be told.

Test tones have the same trap in miniature: `mediaperch-probe play --amplitude` scales the
*generator*, not a decoded stream, so nothing is rounded twice — but a quiet tone exercises
proportionally fewer bits of the container, and the tool now says how many. Eleven of
sixteen, at the amplitude that is comfortable in headphones. The bit-exactness proof is the
capture test, never the tone.

### Is there anything below WASAPI exclusive?

No, and it is worth writing down because the question keeps coming back.

An exclusive-mode stream has no mixer, no APO, no resampler and no volume in front of it —
Microsoft documents that the session volume interfaces have no effect on one. And for a
WaveRT driver, `IAudioRenderClient::GetBuffer` hands the application the hardware buffer
itself: "no system intervention is required to transfer data between an exclusive-mode
application and the audio hardware." There is no lower place for user-mode code to stand.

| | What it would buy |
|---|---|
| **Kernel Streaming** | nothing. Same driver, same buffer. It existed to bypass KMixer, which was a pre-Vista problem |
| **ASIO** | not a lower layer — a different one, with its own driver. No PCM accuracy to gain, because exclusive mode is already exact. What it buys is **native DSD**, without DoP's PCM wrapper |
| **A kernel driver of our own** | a driver to sign, a driver to support, and the same bytes |

The DSD case is the only real one, and it is now bounded: DoP carries DSD256 in 705.6 kHz
PCM, which §14 records as working on a FiiO KA5. DSD512 needs 1411.2 kHz, which is past what
that device accepts as PCM — so native DSD over ASIO is what a DSD512 library would need, and
nothing else is.

That became practical while this was being planned: **Steinberg relicensed the ASIO SDK under
GPLv3 in October 2025**, alongside VST3. Before that it could not be redistributed, which is
why Audacity shipped without it for two decades and foobar2000 keeps it in a separate
component. For a `GPL-3.0-or-later` project it is now simply a module — `sink_asio`, behind
the same vtable as `sink_wasapi`, chosen by the user and absent from the default install.

**Built, and the prediction above held exactly**, on the same FiiO KA5 §14 measured DoP on:

| File | DoP over WASAPI | Native over ASIO |
|---|---|---|
| DSD64 | 176400 Hz | 2822400 Hz |
| DSD128 | 352800 Hz | 5644800 Hz |
| DSD256 | 705600 Hz | 11289600 Hz |
| **DSD512** | **refused** — 1411200 Hz is past what it takes as PCM | **22579200 Hz**, and the DAC's own display read `DSD512` |

That last row is the whole argument for this module, measured rather than reasoned. It also
corrects an expectation: the device is documented at DSD256 and it *is* DSD256 over DoP,
because DoP spends 24 bits carrying 16 and the PCM link runs out first. The native link has
no such overhead and the same hardware reaches twice as far.

**What it cost was one finding about this ABI and one about COM**, both in
[formats.md](formats.md): the sink vtable turned out to be a sink's rather than WASAPI's,
with one adaptation and one extra copy; and an ASIO driver is registered `Apartment` while
this engine is `COINIT_MULTITHREADED`, so `CoCreateInstance` hands back a proxy for an
interface that has never had a marshaller. `sink_asio` loads the driver DLL itself.

---

## 6. Format negotiation

This is the step the architecture exists for, so it is specified rather than left to the
sink module's judgement.

```
  source format (from the decoder)
        │
        ▼
  candidate list, in preference order
        │
        ▼
  for each candidate:  IsFormatSupported  →  Initialize   ← the real test
        │                                        │
        │                                        ├─ AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
        │                                        │     → GetBufferSize, recompute the
        │                                        │       duration from the returned frame
        │                                        │       count, Initialize again  (§14)
        │                                        │
        │                                        └─ AUDCLNT_E_UNSUPPORTED_FORMAT → next
        ▼
  first candidate that initialises wins
        │
        ├─ it is the source format, unchanged   → Path A
        └─ nothing matched                      → ask, per §6.3
```

### 6.1 Candidate order

Candidates are generated over **containers**, not over sample formats, and the source's own
container comes first. For each container that can hold the source's valid bits:

1. the plain form;
2. the same thing as `WAVEFORMATEXTENSIBLE` with an explicit channel mask, because some
   drivers accept only the extensible form even for stereo.

Then the next container, from small to large, so a device that takes several gets the
cheapest wire format rather than the widest. Stop there. A rate change, a channel change or
losing valid bits is a conversion, and a conversion is Path B and a user decision.

**Every container that fits is offered, including ones smaller than the source's.** "24-bit"
names two different wire formats — three bytes packed, and 24 valid bits inside four — and
devices want one or the other, with no way to tell which but to ask. Moving 24 valid bits out
of a four-byte container into a three-byte one drops only the padding, so it loses nothing;
calling it a "narrowing" and refusing it means refusing perfectly playable audio. §14 has the
measurement that established this.

**The mask variant is paired with its own container rather than appended after all of them.**
Written out as prose the rule reads "exact, then other containers, then extensible", which
would try a repacked plain form before an exact extensible one — and so would change the
container needlessly on any driver whose only complaint was the missing channel mask. §14 has
that measurement too. Cheap to get wrong, and invisible afterwards, because the result still
plays.

Non-PCM encodings get no repack at all. Moving a DoP frame between containers shifts its
`0x05`/`0xFA` markers relative to the sample bits and the DAC stops seeing DSD; a bitstream is
not samples in the first place.

`IsFormatSupported` is called first only as a cheap filter. **`Initialize` is the answer**:
in exclusive mode the driver, not the audio engine, decides, and some drivers say yes to
formats they then refuse.

### 6.2 What "bit-exact" means here

Every sample that leaves the decoder reaches `ReleaseBuffer` carrying the same valid bits, in
the same order, with no gain applied and no rate conversion. Moving those bits between
containers, and naming a channel layout the source left unspecified, are permitted. Nothing
else is.

Which splits the passthrough path in two, and the distinction is worth naming because it is
the difference between a `memcpy` and a loop:

| | What the sink accepted | What Path A does |
|---|---|---|
| **exact** | the same container and the same valid bits | `memcpy` |
| **repacked** | the same bits in a different container | keep the top `min(from, to)` bytes, zero-pad the bottom |

Both are bit-exact — no signal lost, no gain, no rate change, no float anywhere. Only the
first is literally a copy. Saying "Path A is one `memcpy`" is true of the common case and
not true of all of it, and the graph has to know which of the two it is running.

Because everything is left-justified — `WAVEFORMATEXTENSIBLE` puts the valid bits at the top
of the container and zero-pads the bottom — a repack needs no arithmetic in either direction.
It is a byte move, and it is lossless going to a *smaller* container exactly when the valid
bits still fit, which is the only thing §6.1 has to check. This was originally a `promote`
that could only widen; the first device that wanted a 24-bit format wanted the four-byte one,
and the second wanted the three-byte one.

### 6.3 When negotiation fails

Three outcomes, and the user picks the default once in settings:

| Choice | Behaviour |
|---|---|
| **Convert** | fall to Path B, loud in the UI about what it did. `--path auto` today, and it prints `PROCESSED -- the samples are changed`. The **resampler** is the part that is not written, so this covers a sample-type change and not a rate change |
| **Shared** | fall to shared mode, ideally with `AUDCLNT_STREAMOPTIONS_RAW` via `IAudioClient2::SetClientProperties` to bypass system effects, and `IAudioClient3::InitializeSharedAudioStream` at `GetSharedModeEnginePeriod` for latency |
| **Refuse** | do not play, and say exactly which format the device declined |

"Refuse" must exist. It is the option that makes the other two honest.

---

## 7. Decoders, and how one is chosen

| Module | Backend | Covers | Why it exists |
|---|---|---|---|
| `decode_native` | C++, `dr_flac` and `dr_wav` single headers | FLAC, WAV | the floor: no build system beyond two `#include`s, so an install with nothing else on disk still plays music. **Measured bit-exact** for 16- and 24-bit, and measurably *not* able to read 32-bit FLAC |
| `decode_flac` | libFLAC, the Xiph reference decoder, as a submodule | FLAC, all depths and rates | for a lossless codec the reference implementation *is* the specification, which is worth a dependency in a way it would not be for a lossy one. Reads what `dr_flac` cannot, and checks its own output against the MD5 the encoder wrote into the file |
| `decode_ogg` | libvorbis and libopus, the Xiph reference decoders, as submodules | Vorbis and Opus in Ogg | the same argument as `decode_flac`, arriving at a different place: these are the reference decoders, so they define what the codec means -- but the codecs are *lossy*, so what they define is a signal, not a byte pattern. This module reports MP_SAMPLE_F32 because that is what they produce, which puts every file it reads on Path B. See *Lossy codecs and Path A* below |
| `decode_mp3` | `dr_mp3`, from the submodule `decode_native` already uses | MP3, every MPEG version and rate | not a better decoder -- it agrees with FFmpeg to 124 dB, which is float rounding for a codec whose conformance is defined as an RMS bound. It exists because Media Foundation does not implement gapless metadata and starts every MP3 36 ms late, and because dr_mp3 does and costs no new dependency |
| `decode_aac` | nothing at all: the codec, the ADTS parsing and the MP4 parsing are all in this tree | AAC-LC in M4A and raw ADTS, every rate and every layout to 7.1 | the same argument as `decode_alac` reaching a different place: not an unmaintained reference, but *four* candidate libraries each producing the wrong thing rather than a wrong sound. See *AAC-LC, which is also written rather than vendored* below |
| `decode_mf` | Media Foundation `IMFSourceReader` | whatever nothing above it read — and WAV and FLAC, **also bit-exact** | ships with Windows and needs nothing installed, which is what it is now for. It began as the answer for MP3 and AAC and is now the last resort for both: it implements gapless metadata in no codec, clips float WAV to integer, scrambles multichannel ALAC, and refuses 8 kHz and 7.1 AAC |
| `decode_alac` | nothing at all: the codec and the MP4 parsing are both in this tree | ALAC in M4A, every depth to 32 bits and every layout to 7.1 | the reference is the specification *and* is unmaintained, so it was read rather than linked. See *ALAC, which is here and is written rather than vendored* below |
| `decode_ffmpeg` | `ffmpeg` and `ffprobe`, **found at run time, never shipped** | WavPack, Monkey's Audio, Matroska, DSF/DFF, OggFLAC, HE-AAC, and whatever else is installed | the fallback, at priority 60: every other module knows its own formats better, and it sits above `decode_mf` because measurement put it there. Not vendored, for the reasons in *Where dependencies come from* below |

Three modules read FLAC, deliberately. `decode_flac` outranks `decode_native` on priority
(120 against 100) so the reference wins whenever it is installed; `decode_native` remains
the answer for an install that wants no submodules at all; `decode_mf` scores 40 because it
reaches FLAC through a pipeline that could insert a converter. Which of those a person wants
is not ours to decide, and the module boundary is what lets it be their decision.

Resolution, in order, and it is written down because "it depends on the config" is not a
design:

1. an explicit choice — `--decoder mf` on the command line, or a per-extension override in
   the config file. An explicit choice wins outright, even over a decoder that would score
   higher, because being able to say "use that one" is the point of having more than one;
2. otherwise every loaded decoder is shown the file's first 4 KB and the best `probe` score
   wins, ties broken by the module's declared priority;
3. first that opens successfully wins; a decoder that fails mid-file does **not** trigger a
   silent retry with another backend, because a half-decoded track that switches backend is
   worse than a clean error.

Implemented in `ModuleRegistry`, which loads every `mp_*.dll` beside the executable rather
than naming the one it wants. `mediaperch-probe claims --file X` prints every decoder's score
for one file, and [formats.md](formats.md) carries the audit of all of them.

**FFmpeg is RECOMMENDED; Media Foundation is NOT RECOMMENDED.** In the key words of RFC
2119: installing FFmpeg is OPTIONAL and exists so that Media Foundation can be avoided,
and using Media Foundation SHOULD NOT be relied on for anything a person cares about the
samples of. It is kept because an install with nothing else on disk still has to play
something, and that is the whole of its remit.

**Media Foundation is last everywhere, and that is a conclusion rather than a preference.**
It scores 20 on everything it recognises, one below `decode_ffmpeg`'s fallback 30, because
every format it reads has been measured and every measurement went the same way: float WAV
clipped, 32-bit FLAC refused, multichannel ALAC in the wrong speakers, gapless metadata in no
codec at all, and a 16-bit default on lossy streams that declare no depth. On a machine with
nothing else installed it is still the answer, because then `decode_ffmpeg` scores 0 and 20
beats nothing — which is the whole of what this module is for.

The choice is per-track and visible: the UI and the log both name the module that opened
the file.

### Where dependencies come from

Two mechanisms, and which one a dependency gets is decided by whether **we** build it.

| | Examples | Why |
|---|---|---|
| **Git submodule, built from source** | `external/dr_libs`, `external/flac`, `external/ogg`, `external/vorbis`, `external/opus`, `external/libebml`, `external/libmatroska`, `external/utfcpp`, `external/Bento4`, `external/mpg123`, `external/wavpack` | Pinned to a commit by the gitlink, so a checkout is reproducible and an upgrade is a reviewable diff. All of them have (or need) no build system of consequence: dr_libs is headers, the Xiph libraries are CMake-native. The tree builds them; CI builds them; nothing is downloaded at configure time except Catch2 |
| **Found at run time, never vendored** | FFmpeg | Its configure is a shell script needing MSYS2 and nasm on Windows, its build is tens of minutes, its output is tens of megabytes, and **its licence is a choice the user should make** — LGPL-2.1+ by default, GPL with `--enable-gpl`, and non-free options past that. Vendoring one configuration decides all of that for them |

The rule generalises: **vendor what you compile, resolve what you don't.** A module that
cannot find its dependency declines every file, which is the behaviour the whole
architecture already has for a module nobody installed:

```
[info ] no ffmpeg/ffprobe on PATH or beside the module; decode_ffmpeg will decline every file
```

**And "resolve at run time" means the executables, not the DLLs.** Loading `avcodec` with
`LoadLibrary` gets the function pointers and leaves the harder half: FFmpeg's public structs
— `AVFrame`, `AVCodecContext`, `AVStream` — change layout between major versions, so the
headers that describe them have to match the binary, and they cannot be vendored on their
own because `avconfig.h` is generated by configure. Going through `ffmpeg` and `ffprobe` as
programs costs a process per file and buys immunity from all of it: one build of this module
works against FFmpeg 4 through 8. `ffprobe` reports the stream's native sample format,
`ffmpeg` is asked for exactly that raw format so that nothing in the chain converts, and the
result is hashed like everything else.

#### On "should every decoder use the official library"

Not automatically, and the reason is not size. Three tests, and a reference implementation
has to pass all three.

**Is it the specification?** For a **lossless** codec the reference implementation *is* the
specification, and a reimplementation can drift from it silently: §14 records `dr_flac`
decoding a 32-bit FLAC to nothing at all. For a **lossy** codec "correct" is a tolerance
rather than an identity, so the argument is weaker — though not absent, since the reference
still defines the tolerance. WAV fails this test in the other direction: there is no
reference implementation to prefer, because there is no reference implementation.

libebml and libmatroska are LGPL 2.1-or-later, which a GPL-3.0 program may
link: LGPLv2.1+ can be taken as LGPLv3, and LGPLv3 is compatible with GPLv3.

Bento4 is the one that has to be read rather than glanced at. "GPL" beside a
GPL-3.0-or-later tree looks like a problem and would be one if it were
GPLv2-**only** -- GPLv2 and GPLv3 cannot be combined. Every Bento4 source file
says *"either version 2, or (at your option) any later version"*, so it may be
taken as GPLv3 and linked here. It is also dual-licensed, with a commercial
licence for anyone who cannot take the GPL; that is not this tree's problem, but
it is why the project's own README says only "dual-license model" and sends you
to a web page.

**Is the licence compatible?** libogg, libvorbis and libopus are all
three-clause BSD, which is GPL-compatible, so they can be linked into a GPL-3.0 program and
the combination stays distributable. Apple's ALAC reference is Apache-2.0, which is
compatible with GPL-3.0 but *not* with GPL-2.0 — worth knowing before anyone relicenses.
For AAC the obvious candidate, FDK-AAC, is not GPL-compatible at all.

**Is it maintained?** This is the test ALAC fails, and it is the one that is easy to forget,
because "reference implementation" sounds like a permanent property. It is not. A decoder
parses a file somebody else wrote, so an unmaintained decoder is an unmaintained parser
pointed at hostile input.

"Maintained" needs a check that is not a vibe, and there is a good one: **is it in
OSS-Fuzz?** Google runs continuous fuzzing against the projects listed there and reports
what it finds to the maintainers, so presence in that list means both that somebody is
looking for these bugs and that somebody is expected to fix them. Checked directly against
`google/oss-fuzz/projects`:

| Library | In OSS-Fuzz |
|---|---|
| `dr_libs` | yes |
| `flac` | yes |
| `vorbis` | yes |
| `opus` | yes |
| `libebml` | yes (LGPL 2.1+) |
| `libmatroska` | yes (LGPL 2.1+) |
| `utfcpp` | yes (BSL-1.0) |
| `mpg123` | yes (LGPL 2.1) |
| `wavpack` | yes (BSD-3-Clause) |
| `Bento4` | yes (GPL-2.0-**or-later**; GPLv2-only would not be) |
| `faad2` | yes |
| `ffmpeg` | yes |
| `libxaac` | yes |
| `alac` | **no** -- and this is the one that is not linked |

Every dependency this tree compiles is on the right side of that line. The one that is not
is the one this tree does not link. Note the shape of the AAC rows: the three libraries this
tree declined are all maintained and all fuzzed, so the third test is not what turned them
down. The next section is what did.

#### ALAC, which is here and is written rather than vendored

ALAC is a lossless codec whose reference implementation Apple open-sourced in 2011 under
Apache-2.0. By the first two tests that makes it exactly the kind of library this section
argues for. It is not linked here anyway, and the third test is why.

Apple has not touched the decoder since 2011. In 2022 Check Point Research published
**ALHACK**: Qualcomm and MediaTek had both ported that code into their audio DSPs, and the
decoder bugs came with it. `CVE-2021-0674` and `CVE-2021-0675` (MediaTek) and
`CVE-2021-30351` (Qualcomm, rated critical) gave remote code execution from a malicious
audio file across roughly two thirds of the smartphones sold in 2021. The chipset vendors
patched their own forks. Upstream never was, and `alac` is the one dependency in this
document that is absent from OSS-Fuzz.

The community fork most people reach for, `mikebrady/alac`, says in its own README that it
is deprecated "due to myriad security issues". `nu774/qaac` vendors the same code and has
touched `ALACDecoder.cpp` four times in fifteen years, most recently in 2022 -- an actively
maintained project around an unmaintained codec, which is the distinction this test is
about. Linking any of them would repeat the mistake that produced ALHACK, in a program
whose whole job is to open files it did not create.

**Nor does running it in a child process fix it.** A subprocess without a sandbox holds the
same user token: code execution there is code execution here. That would relocate the
parser, not contain it. Containment needs a job object, a low-integrity token and no
network -- the out-of-process hosting §12 lists, which does not exist yet.

So `decode_alac` is written here, from the reference read as a specification. That is the
option the three tests actually point at: ALAC is a small codec -- Rice coding and adaptive
LPC, no code books -- it is **lossless, so correctness is self-verifying** (a decode either
reproduces the encoder's input exactly or it does not), and this tree already has a
libFuzzer and ASan harness to point at it. New code has new bugs. The difference is that
they are ours, our fuzzer finds them, and nobody has to be waited for.

What that bought, measured: 13 files from 16 to 32 bits, 1 to 8 channels and 44.1 kHz to
384 kHz, each decoded to a SHA-256 identical to the WAV that was encoded, including the
format's ceiling of 32-bit/384 kHz/7.1. Seeking checked against the tail of a straight
decode. Identical output from MSVC and clang-cl. `fuzz/alac_fuzzer.cpp` drives both the
container parser and the codec.

It is also the only decoder here with **no dependency of any kind** -- no submodule, no
runtime library, no OS codec. `decode_native` is close, but `dr_libs` is still somebody
else's. That is a statement about responsibility rather than about quality.

**The rule this adds: a reference implementation is preferable only while it is
maintained.** After that it is old code with an authoritative name on it, and the choice is
between an independent implementation and writing one.

#### AAC-LC, which is also written rather than vendored

ALAC was written here because its reference is unmaintained. AAC is the harder case, because
nothing about it fails the three tests in the obvious way. Four decoders were available and
all four are maintained; three of them are in OSS-Fuzz. What ended each was measurement:

| | Measured | Verdict |
|---|---|---|
| Media Foundation | starts every AAC track 1024 frames -- 21.3 ms -- late, leaves the encoder padding on the end, refuses 8 kHz and refuses 7.1 | it implements no gapless metadata in any codec, and two formats it will not open at all |
| FAAD2 (GPL-2.0-or-later, compatible) | discards **two** frames where the file's edit list says one | every track begins 1024 frames into the audio. Four different ways of driving the library produced the identical wrong placement, so it is the library's behaviour and not the harness |
| libxaac (Apache-2.0, compatible, in OSS-Fuzz, shipped in Android) | emits 16- or 24-bit **integers** and nothing else | AAC's inverse transform produces real numbers. Taking integers from a decoder is a quantisation performed inside the decoder, where the user did not choose it and cannot see it -- the one thing this project's decoders are not allowed to do |
| FDK-AAC | -- | the licence is not GPL-compatible. The question stops before any measurement |

**Vendoring FFmpeg's AAC decoder alone was considered and rejected.** It is LGPL-2.1, so the
licence permits it, and it is the decoder every measurement here is checked against. But
`libavcodec/aac/` does not stand alone: it reaches into `get_bits.h`, `mdct15`, `sinewin`,
`kbdwin`, `float_dsp`, `mem.h`, the `AVCodecContext` machinery and the fixed/float template
system, and pulling the transitive closure of that is vendoring a slice of FFmpeg rather than
a file. A slice that then has to be tracked against upstream by hand, which is the
maintenance problem the third test exists to avoid, arriving by a different door.

So the codec is in this tree: written in C++ at 1,599 lines plus 368 of generated tables,
and since ported to Rust, bit-identical, at 2,126 with its tests. **SBR and PS are
not** -- another six thousand lines apiece for a profile that is refused at the
AudioSpecificConfig and handed to `decode_ffmpeg`, which is what a fallback chain is for.

**The part that is different from ALAC: correctness is not self-verifying.** A lossless
decode either reproduces the encoder's input exactly or it does not, and that single check
covers everything. AAC has no such check -- ISO/IEC 14496-4 defines conformance as an RMS
error bound, so "correct" is a tolerance and any comparison is a judgement call. Four
independent checks were built instead, and every one of them caught something the others
did not:

- **Bit accounting.** A frame that parsed correctly ends *inside the last byte of its
  packet*. Across 36 files and 3567 packets every frame ends exactly on its boundary with
  zero slack bits. A single misread bit desynchronises a frame and it cannot land on the
  boundary again by luck, so this one check covers the whole parser without a reference
  decoder in sight.
- **Kraft equality** on the twelve Huffman codebooks, checked by the generator that
  transcribes them: a complete prefix code satisfies the sum of 2^-length equalling one, so
  a mistyped table is refused before it is written. The decoder then builds the tries at
  `init()` and fails if any codeword is a prefix of another, which is a different claim and
  the one decoding depends on.
- **The Princen-Bradley identity** on the windows: w[i]^2 + w[N/2-1-i]^2 must be 1 for the
  transform to reconstruct. A misread sine-window formula gave 0.000005, 0.184 and 2.0 --
  the identity found it in one line, and the file that was being tested at the time used
  the KBD window and hid it completely.
- **FFmpeg, sample for sample**, which is where the remaining errors showed up: 134.5 to
  140.0 dB across 35 files, largest single-sample difference 8.9 x 10^-8 anywhere, which is
  -141 dBFS.
  Exact equality is not available in principle -- the inverse transform is an algorithm
  choice, a direct cosine sum in `double` here against a split-radix FFT in `float` there --
  so 135 dB *is* the agreement, and anything below it is a bug.

`docs/formats.md` has the tables. MSVC and clang-cl produce identical output on every file.
`fuzz/aac_fuzzer.cpp` drove the codec from the same input shape `alac_fuzzer.cpp` did:
437,595 executions in five minutes under libFuzzer, nothing found. The fuzzer is
`modules/codec/aac/fuzz` now, in Rust, with the same corpus, and the ADTS framer has one of
its own for the first time.

**The rule this adds: "is it maintained" is necessary and not sufficient.** Three maintained,
fuzzed, licence-compatible AAC libraries were available and each produced the wrong *thing*
rather than a wrong sound -- wrong placement, wrong sample type, wrong refusals. None of that
is visible from a dependency's README; all of it is visible in half an hour with a reference
decoder and a length column.

#### Why the lossy codecs still get their reference libraries

The argument for the reference implementation was made about *lossless* codecs, where
correctness is an identity. It does not apply to Vorbis and Opus, so the question of whether
four submodules are worth it -- rather than handing the files to Media Foundation, which
already ships with Windows -- is a real one. It was measured rather than assumed.

**Media Foundation cannot open Ogg.** It has the Vorbis and Opus decoders and no Ogg
demuxer, so every `.ogg` and `.opus` file is refused by the source reader. Reaching those
decoders would mean writing an Ogg demuxer and driving `IMFTransform` by hand: more of our
code than libvorbis costs, and libvorbis still there. That alone settles it.

The rest is worse. Given the same stream in Matroska, MF ignores the Opus pre-skip that the
specification requires it to discard, so the first 648 samples of a track are wrong and 648
more are appended -- 13.5 ms of wrong audio at every track start, and gapless playback
impossible. It pads Vorbis lengths by 128 to 832 samples. It refuses multichannel Opus.
FFmpeg, reading the identical file, is exact in every one of those cases. `docs/formats.md`
has the numbers.

So the distinction is not lossless-versus-lossy after all. It is this: **use the reference
when it is maintained and the codec is too large to own; write it yourself when the codec is
small enough that you can; and use the OS decoder when it is measurably right.** ALAC met
the second test -- Rice coding and adaptive LPC, and an abandoned upstream. Vorbis and Opus
meet the first: libvorbis and libopus are maintained, both are in OSS-Fuzz, and neither is a
codec anybody should reimplement for fun. Media Foundation meets the third for WAV and
FLAC alone: it fails it for Ogg, for ALAC past stereo, and -- once gapless metadata was
measured rather than assumed -- for MP3 and AAC as well.

`decode_ogg` is also portable, which `decode_mf` is not, and it is a module: an install that
wants neither the submodules nor the four hundred kilobytes simply does not build it and
gets Vorbis and Opus from `decode_ffmpeg` instead.

#### Lossy codecs and Path A

`decode_ogg` reports MP_SAMPLE_F32 and there is no version of it that does otherwise.
libvorbis and libopus are float codecs; `ov_read` and `op_read` reach int16 only by
converting, and a decoder in this tree does not convert — that is the graph's job, on Path B,
where it is visible and where the user chose it.

The consequence is that no lossy file takes Path A, and that is not a limitation of this
implementation. It is also why Path B exists at all: the endpoint on the development machine
refuses `F32` in exclusive mode, so before there was a Path B every MP3, AAC, Vorbis and
Opus file there could be decoded and hashed and could not be played.
`mediaperch-probe negotiate --float` is the one-line way to ask any device the same
question, and `--path auto` is the answer when it says no. A lossy codec's output is *defined* as a floating-point signal with a
tolerance; there is no byte pattern for it to be bit-exact to. Rounding to S32 inside the
decoder would produce something that looked like Path A material and claim an exactness that
exists nowhere in the chain. Measured against FFmpeg's own independent decoders, this module
agrees to 130–140 dB SNR — float rounding, and about as close as two implementations of a
tolerance-defined codec can get.

---

## 8. Clock and A/V sync

**The audio device is the master clock.** `IAudioClock2::GetDevicePosition` plus
`IAudioClock::GetFrequency`, correlated with `QueryPerformanceCounter`, gives the
presentation time everything else follows.

The consequence has to be stated, because it is the opposite of the usual answer: **in
exclusive passthrough there is no resampler, so audio cannot be rate-matched to video.**
Video therefore drops or duplicates frames against the audio clock, always. Never adjust
audio to keep video smooth — that is a conversion, and this player does not do conversions
it did not announce.

For audio-only playback the clock is used for gapless boundaries and for the position
readout, and nothing else reads it.

---

## 9. Video and HDR

The differentiating feature, and the one that needs the most care because the platform
documentation contradicts a widely-held assumption.

### 9.1 Two different stages, and only one of them tone-maps

Windows tone-maps HDR video for SDR displays in *one* place, and it is not the place most
people assume. Getting these two stages the right way round is the whole of this section.

| Stage | What it does with out-of-range HDR | How you reach it |
|---|---|---|
| **Video processing**, before composition | **tone-maps.** This is what the *Stream HDR video* setting turns on, and why HDR content looks acceptable on an SDR panel in Movies & TV, Netflix or Edge | the GPU video processor, or the Media Foundation playback pipeline. **Not automatic** — you have to route the frame through it |
| **DWM composition**, after | **clips.** An Advanced Color swap chain targeting a display without those capabilities is numerically clipped; everything outside `[0, 1]` in an FP16 scRGB buffer is simply lost | unavoidable — this is what presentation does |

The consequence for a player: **decode with FFmpeg, upload your own texture, present it, and
nothing tone-maps it** — the system setting does not help, because the frame never went
through the stage that honours it. The `driver` provider below is exactly how a custom
renderer opts back into that stage.

### 9.2 The OS tone mapper is good, free, and measurably wrong

Worth knowing before treating it as a reference: the *Stream HDR video* conversion has a
long-standing, vendor-acknowledged defect. It maps the PQ EOTF to a **2.4 gamma** rather
than to the sRGB piecewise curve or BT.1886 that Windows uses for SDR everywhere else. On an
sRGB-calibrated display that means washed-out midtones with crushed blacks, at the same
time. Intel published a support note titled "HDR to SDR Conversion for Windows' *Stream HDR*
Function Is Incorrect"; there is no fix on an internal panel short of a 3D LUT through the
DWM, and external monitors only work around it by having their own gamma control.

So "it looks fine" and "it is correct" are both true statements about different things, and
the design follows from that: **default to the OS mapper because it is free, hardware
accelerated and matches every other Windows app, and keep a correct one selectable.** Ours
targets sRGB with a BT.2390 EETF — which is, precisely, what those bug reports have been
asking Microsoft to do.

### 9.3 The providers

"The OS tone mapper" is real; it just has to be called:

| Provider | API | Runs on |
|---|---|---|
| `driver` | `ID3D11VideoContext::VideoProcessorSetOutputColorSpace` + `VideoProcessorSetStreamColorSpace1`, with `ID3D11VideoContext2::VideoProcessorSetStreamHDRMetaData` | the GPU's fixed-function video processor. The driver tone-maps using the stream's HDR metadata |
| `d2d` | the Direct2D HDR tone map effect | Direct2D. The same tone mapper Windows ships for its own HDR video pipeline |
| `shader` | our own BT.2390 EETF | our pixel shader. The fallback, and the escape hatch when a driver's is bad |
| `none` | — | the display is already HDR; pass PQ through |

`driver` is the default — it is the cheapest, it is what the OS's own player path uses, and
it is the answer to "why does this look like Windows and MPC-BE does not".

### 9.4 Detecting what the display actually is

| Windows | Call | Gives |
|---|---|---|
| 11 24H2+ | `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2` | the **active colour mode** — SDR / WCG / HDR — for a desktop app, and `DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE` to toggle HDR with the user's consent |
| 10 2004+ | `IDXGIOutput6::GetDesc1` | HDR yes/no, plus the ST.2086 colour volume. **Cannot distinguish an auto-colour-managed SDR display from a plain one** — both report `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` |
| any | `QueryDisplayConfig` + `DISPLAYCONFIG_SDR_WHITE_LEVEL` | the user's SDR reference white in nits, needed for §9.6 |

`Windows.Graphics.Display.AdvancedColorInfo` is the nicest of these and is **UWP only** — a
desktop app without a `CoreWindow` cannot use it. Do not plan around it.

Capabilities change while running: the user toggles HDR, or drags the window to another
monitor. Poll `IDXGIFactory1::IsCurrent` each frame, handle `WM_SIZE`, and on a change pick
the output with the greatest intersection with the window rather than calling
`IDXGISwapChain::GetContainingOutput`, which returns a stale output and whose obvious fix —
recreating the swap chain — flashes black.

### 9.5 Presentation

Flip model, always (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), because that is what makes a swap
chain eligible for Advanced Color processing at all.

- **General path:** `DXGI_FORMAT_R16G16B16A16_FLOAT`, scRGB
  (`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). Works on every display kind, blends with the
  OSD, costs 64 bits per pixel.
- **Fullscreen HDR10 optimisation:** `DXGI_FORMAT_R10G10B10A2_UNORM` with
  `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)`. Half the
  bandwidth, but no alpha blending — so only when nothing is composited over the video.

**And fullscreen is worth reaching for its own sake**, which this section did not
previously say: a fullscreen flip-model chain gets *independent flip*, where the
display controller scans the swap chain's buffer out directly and the DWM composites
nothing. It removes a copy and about a frame of latency. It does **not** move the
precision ceiling — §9.10 — because the buffer being scanned out is still a swap
chain buffer in one of the same four formats.

### 9.6 The one that will look wrong first

On an **HDR** display, scRGB `1.0` means 80 nits — *scene-referred*. On an Advanced Color
**SDR** display, `1.0` means the display's reference white — *display-referred*. Subtitles
and the OSD drawn at `1.0` on an HDR display therefore appear at 80 nits, which is dim and
grey next to the video, and this is the single most common HDR bug in players.

Fix: read the SDR white level (§9.4), and multiply SDR content by
`sdr_white_level_nits / 80` in linear space before compositing. Alternatively, render the
OSD to its own surface and let the OS composite it — Windows then applies the boost itself.
The second is less code and is what the plan does; the first is kept for the fullscreen
HDR10 path where there is no second surface.

### 9.7 Order of work

Present the OS path first (`driver` tone mapping, hardware decode through an MFT, flip-model
scRGB) and get it correct. Only then add `shader`, and only as an option — a hand-written
tone mapper that ships before the platform one has been made to work is how a project ends
up maintaining a colour pipeline it never meant to own.

**And before any of it, a way to tell.** §9.2 is the record of what happens without one:
Windows maps PQ to a 2.4 gamma where the sRGB curve belongs, Intel published a support note
saying so, and it has survived years of that because the result looks fine. A colour pipeline
judged by whether it looks plausible is not judged.

So `MpVideoVtbl::open` takes NULL for a window and renders to a texture, `read_back` hands
the pixels over as 8-bit sRGB, and the presenter is measured the way every decoder in this
tree is measured -- by hashing what came out. **WARP is asked for by name** in the tests
rather than tolerated as a fallback: Microsoft's software rasteriser is on every Windows
install and is deterministic, so the bytes are the same on a machine with a GPU, a machine
without one, and a CI runner. `read_back` is also the screenshot people want, which is why it
is one call rather than a diagnostic build.

The decisions themselves are `modules/video/d3d11/colour_plan.hpp`, deliberately apart from
any Direct3D: which swap chain format, which tone mapper, and what SDR content is multiplied
by all follow from what the container said and what the display is, and none of them needs a
device to work out. They are the part that is easy to get subtly wrong, and they are tested
against the cases this section names.

### 9.10 How many bits, and where they are lost

Asked because a display may be 10-bit, or 12, and answering it turned up three places where
this tree was throwing precision away.

**Presenting is capped at FP16, that is DXGI's ceiling rather than a choice, and it is not
enough.** A flip-model swap chain accepts `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM`,
`R10G10B10A2_UNORM` and `R16G16B16A16_FLOAT`, and nothing above them.

The first version of this paragraph said half was fine because it exceeded every consumer
panel, which is the wrong yardstick for a video engine that may end up doing colour grading:
if a 16-bit output is conceivable then rounding to something narrower is a decision, and a
decision needs a number rather than an audience.

Here is the number. Half is *relatively* precise -- its step is between 1/2048 and 1/1024 of
the value, everywhere. An N-bit encoded output with a gamma near 2.4 needs a relative linear
precision of `2.4 / (2^N - 1)` at white, and harsher than that below it. So:

| Output | Needed at white | Half is short by |
|---|---|---|
| 8-bit | 9.4e-3 | no -- ten times finer |
| 10-bit | 2.3e-3 | no -- twice finer |
| **12-bit** | 5.9e-4 | **1.7x** |
| 14-bit | 1.5e-4 | 6.7x |
| 16-bit | 3.7e-5 | 27x |

**Half is already short at twelve bits**, not at some hypothetical future panel, and it is
short over most of the range rather than only at white -- the requirement gets harsher as the
signal darkens, until sRGB's linear toe. Where half *is* better than an integer format is the
deep shadows, by an order of magnitude, which is why it suits a linear light buffer at all.
That is a reason to prefer it over `R10G10B10A2` at the same width, not a reason to call it
sufficient.

What it costs is measured rather than assumed: a test renders the same frame at FP32 and at
FP16 and holds the difference to one unit in half's last place, so the table above is a
property of the code rather than of this document.

#### Getting past it

There is **no route above FP16 on the desktop**, and the reason is structural rather than an
API gap. DirectComposition composites DXGI surfaces and takes the same four formats;
fullscreen exclusive takes the same four; the kernel-mode `D3DKMT` thunks present the same
allocations, and the display DDI carries no wider present format for them to name. And
underneath all of it, **the DWM's own composition space is FP16 scRGB** when Advanced Color
is on -- Microsoft documents that -- so a wider surface handed to it would be rounded to half
before it reached a cable regardless.

#### Taking the desktop over instead

The obvious next question, and the answer is no in three separate ways that are worth keeping
apart, because two of them sound like they might be yes.

**The compositor cannot be replaced, and switching it off is not a route to more bits.**
`DwmEnableComposition` was deprecated in Windows 8 and has been a no-op ever since. An
earlier version of this paragraph said there was no unsupported route either, which is not
true: people have renamed or broken `dwm.exe` and run without it, and a surprising amount of
Windows keeps working. What that gets you is the basic display path, which is **8-bit**. So
the objection is real and the direction is wrong -- going around the compositor by killing it
lands below where it started, not above.

**It can be bypassed, and that buys latency rather than bits.** A fullscreen flip-model swap
chain gets *independent flip*: the display controller scans the swap chain's buffer out
directly and the DWM composites nothing. That is real, it is worth having, and it removes a
copy and a frame of latency -- **but the buffer it scans out is still a DXGI swap chain
buffer**, so it is still one of the same four formats. The ceiling does not move. Worth
noting the arithmetic here: our own FP32-to-FP16 store is the one rounding either way, and
the DWM compositing FP16 into FP16 was never adding a second.

**Owning the driver would not help, and it is the interesting one.** Suppose the objection is
taken all the way and a WDDM display miniport is written. What the display controller can
scan out is 8-bit, 10-bit and 16-bit integer, and FP16. **No display controller scans out
single precision** -- there is no such thing to reach. The one thing owning that layer would
buy is `R16G16B16A16_UNORM`, which is a real scanout format, is genuinely better than half for
a 16-bit output, and is *not* a valid swap chain format, so DXGI cannot ask for it. That is
the entire gap a driver would close: sixteen bits of integer instead of eleven of mantissa,
on hardware that may or may not support it, in exchange for writing and signing a kernel-mode
display driver. It is not a trade a media player makes, and it is a smaller prize than it
sounds like -- 16-bit UNORM in a *linear* buffer is worse than half in the shadows, so it
would have to be an encoded buffer, which moves the tone mapping into the driver.

So the honest shape of it: **the desktop's ceiling is not a Microsoft policy that a
determined implementation can go around. It is where the scanout hardware stops.** What
changes the answer is different hardware -- which is the next paragraph, and is why it is a
module rather than an argument.

#### The same question on the other two platforms

Asked because this tree is going to be ported, and the answer is not the same everywhere --
which is itself the argument for the presenter being a module and for the decision that
reaches it being free of any one platform's format list.

| | Widest it will present | Past the compositor? |
|---|---|---|
| **Windows**, DXGI | FP16 (`R16G16B16A16_FLOAT`) | independent flip, same formats. No wider route |
| **Linux**, Wayland + KMS | FP16 *and* **16-bit integer** (`DRM_FORMAT_ABGR16161616`) | **yes** -- a DRM lease drives KMS directly |
| **macOS**, CAMetalLayer | FP16 (`rgba16Float`) | no |

**Linux is the one where the ceiling actually moves.** A Wayland client hands over a dmabuf
with a DRM FourCC, and that vocabulary includes `ABGR16161616` -- sixteen bits of integer per
channel -- which DXGI has no equivalent of at all and which hardware planes on several GPUs
will scan out. On top of that, `wp_drm_lease_v1` lets an application take an output away from
the compositor entirely and drive KMS itself, which is how VR headsets work and which Windows
has nothing comparable to. A compositor can also put a client buffer straight onto a plane
without compositing it, the way independent flip does.

**macOS is the most closed of the three.** A `CAMetalLayer` presents `bgra8Unorm`,
`bgr10a2Unorm`, Apple's `bgra10_xr` and `rgba16Float`, and there is no fullscreen-exclusive
path, no display lease, and no way past WindowServer. Half is the ceiling and it is a harder
ceiling than Windows'.

**What this means for the code, and it is the reason for a refactor rather than a note.**
`colour_plan.hpp` decides §9's questions for every platform, and it had DXGI's format list
inside it: an enum of `{ fp16_scrgb, rgb10_hdr10 }`, which is Windows' four formats minus
two. Portable logic that names one platform's formats makes every other platform round to
them -- and Linux is precisely the platform where half is not the widest thing available. It
decides an **encoding** now (linear scRGB, or PQ) plus whether the buffer has to blend, and
each presenter maps that to the widest format its own platform offers. `dxgi_format_of` is
the only function in the Windows module that knows a DXGI format, and its comment says what a
Wayland or Metal one would answer instead.

The route that exists is the one the audio side already took: **do not use the desktop
path.** `sink_asio` is in this tree because ASIO goes around the Windows audio engine, and it
is a module beside `sink_wasapi` rather than a change to either. A presenter on a dedicated
video output -- Blackmagic DeckLink, AJA Kona -- goes around the compositor the same way,
takes 10-bit or 12-bit straight from a buffer over SDI or HDMI, and would render **FP32
directly to the card's integer format, never touching half at all**.

**The architecture already permits that with no ABI change**, which is worth stating because
it is the thing that would have been expensive to discover later. `MpVideoVtbl::open` takes
NULL for a window, which is what a card wants; `set("device", ...)` already exists and
already carries a value (`warp` or `hardware`), so `decklink:0` needs no new entry point; and
the off-screen target is already `R32G32B32A32_FLOAT`, which is what such a module would
quantise from.

Worth being honest about the ceiling on that path too: SDI carries 10 or 12 bits and HDMI's
deep colour modes define up to 16 but nothing implements them, so **12-bit is the widest a
display link actually delivers today**. The gap half leaves at 12 bits is 1.7x, and a card
path closes exactly that.

And for the use this ceiling was raised about -- grading, analysis, anything whose output is
a file rather than a panel -- the relevant path is `read_back`, which is `R32G32B32A32_FLOAT`
and has no ceiling at all.

**Everything before the store is single precision**, and stays there. HLSL `float` is 32-bit
and this module uses no `half` and no `min16float`, so every transfer function is computed at
full single precision and rounded exactly once, when it is written.

There is **no intermediate render target yet**, and the first version of this paragraph gave
the wrong reason for that -- it claimed an intermediate would round twice. It does not: the
shader writes single precision into it exactly and the copy to the back buffer rounds once,
which is the same one rounding as writing straight there. What an intermediate costs is a
full-screen copy, and what it buys is **a windowed session that can still read back single
precision** -- monitor at what the display takes, export at what the arithmetic produced,
which is precisely what a grading tool wants. It is not built because nothing here opens a
window yet, and off-screen already renders FP32 directly, which is the same thing without the
copy. It goes in with the first window.

**Off-screen renders into `R32G32B32A32_FLOAT`**, which a swap chain will not take and a
measurement should not do without: a test that hashes an FP16 buffer is measuring the
presentation format as much as the pipeline. `precision=fp16` asks for what a display gets,
which is how the paragraph above is measured at all.

**Double precision belongs on the CPU, where constants are derived.** A colour matrix or an
EETF parameter worked out in `double` and rounded once into the constant buffer is exact to
more digits than any display has; in a shader it would buy nothing measurable, since no
texture format carries it and single precision already has seven decimal digits against a
16-bit panel's five. It is the same rule the audio side follows, where the shaping curves are
transcribed at full precision and the bus is f64 because the argument for Path B is that it
rounds exactly once.

Three things were losing bits before this was asked, and all three were in code written the
same week:

- **`read_back` handed back 8-bit sRGB.** It quantised away the dark end, which is where the
  difference between the sRGB curve and §9.2's 2.4 gamma actually lives and is the whole
  reason the measurement exists. On the HDR10 path it was worse than lossy: it read PQ code
  values as though they were linear and produced bytes that were neither.
- **It clamped to [0, 1] before encoding.** scRGB above 1.0 is not an error -- it is how the
  format represents brighter than 80 nits -- so every bit of HDR headroom was being thrown
  away by the one call that exists to inspect it.
- **`demux_mp4` truncated the 16.16 display size** rather than rounding it, losing the
  fraction an anamorphic track states. It rounds now.

### 9.8 Media Foundation is a codec here, not a pipeline

This paragraph used to read "`decode_mf` hardware decode", which was written before ABI v2
and names a module that no longer exists. Restoring it would undo the thing v2 was for.

**MF is two libraries wearing one name.** `IMFSourceReader` is a whole pipeline -- it opens
the file, demuxes it, decodes it and hands back samples, and there is no seam in it. That is
what `decode_mf` was, and it is why v1 could not say where a frame came from.
`IMFTransform` is one decoder: bitstream in, frames out, no file, no container, no seeking.
`MFTEnumEx` with `MFT_ENUM_FLAG_HARDWARE` finds the vendor's, and
`IMFDXGIDeviceManager` hands it the D3D11 device so it decodes straight into textures this
process already owns.

The second is a **codec** in exactly the sense §4 means, so the video path keeps the shape
the audio path has:

```
  demux_mp4 / demux_mkv ─▶ codec_mft (an IMFTransform) ─▶ video_d3d11
   ours, fuzzed, seeking    the black box, one job wide     ours
```

What that buys is not tidiness:

- **The container stays ours.** Bento4 and libmatroska are fuzzed, and their seeking is
  measured to land byte-identically in four framings. MF's is neither, and is not
  inspectable.
- **The HDR metadata comes from the container**, which is where it is written. §9.1 turns on
  knowing the stream's primaries, transfer and matrix before a frame is drawn; asking MF for
  its opinion of them adds a layer that can be wrong with nothing to check it against.
- **The black box shrinks to one function.** A decoder that turns a bitstream into a texture
  is a thing whose output can be held against another decoder's, which is what
  [formats.md](formats.md) already does for every audio codec here.
- **`demux_mf` stays what it is:** a *fallback demuxer* flagged `MP_STREAM_SELF_DECODES`,
  which is the honest v2 shape for the SourceReader and is right for the formats nothing
  else here reads. It is not the video path.

The alternative below MF is D3D11VA directly (`ID3D11VideoDevice::CreateVideoDecoder`),
which is what FFmpeg's hwaccel does. It means parsing slice headers to fill DXVA buffers --
which is precisely the work an MFT already does, correctly, for every codec the GPU
supports. Sitting on the MFT layer is not a compromise; sitting on the SourceReader layer
would be.

### 9.9 What a video packet's timestamp is counted in

**Answered, and it took three more answers with it.** `MpPacket::frame` was documented as
"in the stream's own frames", which an audio stream has and a video stream does not --
24000/1001 of a second is not a unit anything divides evenly. The two demuxers that read
video answered differently and neither could be checked: `demux_mkv` declined to timestamp a
video packet at all, and `demux_mp4` handed back a number in a timescale nothing revealed.

`MpVideoInfo::timescale` is the answer: **ticks per second, for this stream**. An MP4 track
states it in `mdhd` and it is typically the frame rate's numerator, so 24000 here; Matroska
stores a scale and libmatroska hands back nanoseconds, so a Matroska stream reports
1000000000. Ticks per second rather than a rational seconds-per-tick because that is the form
both containers store, and an integer cannot round it. It is an **append** to a struct added
one commit earlier, which is what the size prefix is for -- and a test now asks for the older
size and checks the bytes past it are untouched, because that promise had never been
exercised.

Nanoseconds everywhere was the alternative, and it throws away the exact rational the
container stated, which is how a player drifts. A frame index was the other, and it only
exists for constant frame rate.

#### Three things that fell out of answering it

**Presentation, not decode.** MP4 was reporting `GetDts` and Matroska reports the
presentation timestamp, so the same field meant two things. A stream with B-frames is stored
in an order that is not the order it is shown in -- both fixtures here are, at `-g 6` -- so
the two numbers differ per packet and only one can be compared against §8's audio clock.
`demux_mp4` reports `GetCts` now. For audio the two are the same number, so nothing on the
measured path moved.

**`MP_PACKET_SYNC` was claimed unconditionally.** True of every audio codec here and of no
video one. It was harmless while only audio came through and would have made a video seek
land on a frame that cannot be decoded from. `demux_mp4` reports what the sample says, and
its seek walks back to the nearest sync sample at or before the target -- which for audio is
the same sample, so again nothing measured moved.

**The edit list was read for audio tracks only.** `read_edit` sat inside
`if (kind == MP_STREAM_AUDIO)`, so a video track's `skip_frames` was zero. Two tracks state
different edits: in the fixture, 1024 of 44100 for the audio and 2002 of 24000 for the video
-- **sixty milliseconds of difference**, which is an A/V desync that would have been blamed
on the clock. Timestamps stay container-relative and the edit stays in `MpStreamInfo`, which
is where audio already had it, so the host subtracts it in one place rather than each
demuxer folding it in differently. That is also why these numbers are 2002 ticks higher than
ffprobe's, which folds it in.

#### And the seek that landed one frame late

Measured at exactly one frame on the fixture, which is what made it worth closing rather than
noting: the sample table indexes **decode** time and the target is a **presentation** time,
so a frame whose composition offset pushes it past its own decode slot was the earliest one
the seek delivered. For video that is the direction that cannot be recovered -- a frame
arrived after the point somebody asked to start at.

The fix is to move the target back by the track's largest composition offset before the
lookup, and the reasoning is a two-line one worth writing down. Any sample worth keeping has
`cts >= target`; `dts = cts - delta >= target - reach`; so the sample containing
`target - reach` is at or before every sample the seek must deliver. Starting earlier than
that only costs frames the host discards anyway.

Finding the reach is a walk, because `AP4_CttsAtom` keeps its run-length entries private and
offers only a per-sample lookup -- but that lookup caches its cursor, so a forward pass is
amortised constant per sample, and it happens **once per track, on the first seek** rather
than at open, because most files are never seeked at all.

One guard, for a reason that is Bento4's rather than this tree's: **it reads the composition
offset unsigned**, and the branch that would handle a version-1 `ctts` with negative offsets
is commented out upstream. Such a file yields a reach near 2^32, which would turn every seek
into a seek to zero -- a second failure stacked on the first, since `AP4_Sample::GetCts` is
already wrong for it. A composition offset longer than the whole track cannot be one, so that
is where the guard sits.

The audio path is untouched throughout: no audio codec here has a composition offset, so the
reach is zero and the arithmetic is a subtraction of nothing. A fragmented file states its
offsets in `trun` and has no `ctts` in `stbl`, so it answers zero too -- which is honest,
because its seek is the millisecond-granularity one that already rounds down.

---

## 10. Shell and IPC

The engine is a headless process with no toolkit linked in. Shells attach.

- **Transport:** a named pipe on Windows with a message framing that is a versioned struct
  stream, not a text protocol. One Unix-domain-socket implementation later for Linux.
- **Surface:** playback state (transport, position, current track, current graph, current
  device, the resolved format), the playlist, the settings tree, and a log tail. That is
  all. The shell cannot reach into the graph.
- **Fallback:** the engine keeps a minimal Win32 tray menu of its own, exactly as
  DragonPerch's daemon does, so an install with no shell is usable rather than headless.
  Greying out "Settings" when no shell is installed is the honest behaviour.
- **The shell is killable at any moment** and playback does not notice. That is the whole
  reason for the split, and it should be an actual test: kill the shell mid-track, assert
  zero glitches.

Because the surface is small and versioned, a third-party shell — a web UI, a hardware
remote, a Linux Qt shell — is a normal thing to write rather than a fork.

---

## 11. Configuration

One INI-shaped file, read by the head, validated against a schema that lives in the core, so
that the same file is meaningful on a platform that does not exist yet. Reuse DragonPerch's
INI parser and its fuzz corpus rather than writing a second one.

**Done, and the schema turned out to be somewhere better than planned.** There is no second
schema: `[player]` in the file is a list of arguments to `Player::set`, which is the one
place that decides what a setting means, so `mediaperch-cli set path processed` and
`path = processed` in the file cannot come to disagree. `[engine]` holds the four things a
player cannot own, because they are needed before there is one: where to listen, where the
modules are, which of them may load, and which decoder to prefer.

DragonPerch's parser is a submodule and its INI corpus is what `settings_fuzzer` runs on --
two parsers in one house would have had two sets of edge cases and only one of them fuzzed.
Its `OnBadLine::skip` is the behaviour, for the reason its own header gives: in a file people
edit by hand, losing every setting to one typo is worse than losing the setting the typo is
in. A line that cannot be read is named, with its number, in the log.

`mediaperch-cli save` writes the file back, and what it writes it can read: the round trip is
checked by a test and by the fuzzer.

Settings that matter enough to name here: the exclusive/shared default, the negotiation
failure policy (§6.3), the decoder preference order (§7), the module search path and
allow-list, the tone-map provider (§9.3), and whether the engine may toggle the display's
HDR state.

---

## 12. Testing

| Layer | How |
|---|---|
| core | Catch2, with a null sink and a synthetic decoder. Negotiation, graph selection, ring behaviour, gapless boundaries — no device, no COM |
| bit-exactness | two of them, because they prove different halves. In `tests/`, a fake sink implemented behind the real C vtable records every byte committed and compares it with the source — no hardware, runs in CI. On real hardware, `mediaperch-probe verify` plays a file through a **tee** that copies every buffer handed to `IAudioRenderClient::ReleaseBuffer`, and compares SHA-256 with what the decoder produced. **`ReleaseBuffer` is the boundary the claim is about**, and §14 records why nothing on this machine can see past it |
| decoders | against a reference. `mediaperch-probe decode --file X` prints SHA-256 of the PCM; `ffmpeg -i X -f s16le out.raw` prints the same thing if the decoder is right. Now generated rather than listed: `ctest -R format_matrix` shows one file per format to every decoder and rewrites the README's table -- 22 formats, 61 decoder-format pairs that open, 38 of them bit-exact against the reference |
| decoders, against the audio that was encoded | the check above compares decoders with each other, which cannot answer *are they both wrong*. `mediaperch-probe compare` holds a decode against the uncompressed file that went into the encoder -- length, alignment, channel order, per-band energy and a fidelity floor -- and `cmake/DecodeQuality.cmake` drives fifteen rows of it. [formats.md](formats.md) has the numbers and the three bugs that were put back to prove the check works |
| ring | a soak test with a producer and a consumer under TSan, plus an assertion that the render side never allocates (an allocator hook that aborts while the RT flag is set) |
| parsers | libFuzzer on every one, with corpora in `fuzz/corpus`, as DragonPerch already does. **Eight targets, and they run in CI on every push** — dr_wav, libFLAC, libmpg123, Bento4 and the INI parser in C++ under ASan, and the ALAC and AAC decoders and the ADTS framer in Rust, coverage-guided on the stable toolchain. Thirty seconds each, which is a smoke test that the campaign still builds rather than a campaign, and somewhere for a regression corpus to live. §2 chose C++ for the parsers on the argument that fuzzing closes the gap; an unrun fuzzer would have made that argument worthless, and the three that are Rust now close it a second way |
| properties | randomised invariants over the whole format space, in `tests/`, with a fixed-seed generator so a failure prints a seed that reproduces it. About 15,000 cases per run, no hardware, no Clang: every candidate list is bit-exact and free of duplicate wire formats, non-PCM encodings are never repacked, and `repack` round-trips for every container pair that fits and refuses every pair that does not. This is the cheap half of fuzzing, and it runs on both compilers |
| devices | a manual matrix, because it cannot be automated, kept in [devices.md](devices.md): onboard codec, a USB DAC, HDMI to a receiver, Bluetooth. For each, the formats that negotiated, whether `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` appeared, and the minimum period that ran glitch-free for an hour |
| glitch counting | the engine counts underruns, `AUDCLNT_E_DEVICE_INVALIDATED`, and late render callbacks, and shows them. A player that cannot tell you it glitched cannot be trusted when it says it did not |

---

## 13. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | Repository skeleton, CMake presets, CI | `core` builds alone with the platform directories off the include path, and CI fails if that stops being true |
| M1 | WASAPI exclusive, event-driven, sine from memory | a 1 kHz tone plays for an hour at the minimum device period with zero underruns; the realign path in §14 is exercised deliberately |
| M2 | Module ABI v1 + `decode_native` + `sink_capture` + the two throwaway ABI probes (§2) | **done, and since superseded by M4.5** -- `decode_native` was split into demuxers and codecs and no module by that name is left. `decode_native` (WAV and FLAC, `dr_wav`/`dr_flac`) decoded to hashes identical to FFmpeg's; the fake sink in `tests/` and the tee in `verify` both prove the bytes reach the device unaltered. Both ABI probes are written and run: a C11 module and a Rust `cdylib` produce identical frame counts through the same vtable, and a panic thrown on purpose inside the Rust one is contained at the boundary and comes back as `MP_ERR_INVALID`. See [abi/README.md](../abi/README.md) |
| M3 | `mediaperch-cli` and the IPC | **done.** `mediaperchd` is the engine and has no toolkit in it; `mediaperch-cli` drives it over a named pipe with a versioned binary framing. `mp::Player` is in the core, so the whole engine is tested with no COM and no hardware, and `IEngineHost` is the four things it asks an operating system for. The row is done when killing the shell mid-track is inaudible, and that is a test: three shells attached, subscribed, and cut off, with the underrun count still zero. Both of the things this row was last waiting on are in: §11's INI file, which round-trips through its own fuzzer, and the Win32 notification icon §10 asks for -- play and pause, previous, next, stop, and Settings greyed out with *why* when no shell is installed, because a menu item that silently does nothing reads as a bug in the engine. `--no-tray` is what a service wants. `Tray::run` is the engine's main loop when there is an icon, on the thread that otherwise has nothing to do, and it reaches `mp::Player` through the same commands a shell uses |
| M4 | Path B: f64 bus, DSP chain, resampler, dither. Gapless, seek | **done**, and the passthrough path still contains no float. Gapless is `mp::Queue`, a source whose `read` does not stop at a track boundary; seek and pause are on both graphs; every DSP stage can be told to forget where it was. A device that is taken away (`AUDCLNT_E_DEVICE_INVALIDATED`) is a rebuild rather than an ending, and so is switching *paths*: both resume on the frame the device stopped on, which is the only thing a rebuild point can honestly promise and is checked byte for byte |
| M5 | `decode_mf` and `decode_ffmpeg`, and the resolution table | **done.** `ctest -R format_matrix` builds one file per format, shows it to every decoder, and rewrites the matrix in the README -- and fails when the README stops matching. `mediaperch-probe claims` shows every decoder's probe score for a file, so a cell can say whether a decoder *claimed* the file or was forced to try. The lossless corpus comes from the reference encoders rather than FFmpeg, whose FLAC encoder writes 24 bits when asked for 32. Generating it found two claims in [formats.md](formats.md) that had gone stale and one real gap: nothing but Media Foundation claimed WMA |
| M4.5 | ABI v2: the container decides (§12) | **done.** Every format this tree reads resolves container-first: eight demuxers and seven codecs, and each one decodes to the hash its v1 decoder produced. Two formats gained a first-class reader on the way -- MPEG layer II, which had gone to FFmpeg, and OggFLAC, which `demux_ogg` had been naming since step 4 with nothing to hand it to. Seeking became the host's, once, rather than each decoder's separately: a seek to an arbitrary sample lands byte-identically in WAV, native FLAC, OggFLAC and ALAC-in-MP4, which are four unrelated framings. Modules are laid out and installed by kind -- `modules/<kind>/<name>` in the tree, `bin/<config>/modules/<kind>/` out of it. Step 7 deleted `MP_KIND_DECODER`, the eight modules that used it, `mp::Decoder`, the registry's second resolution path, and one submodule that had no caller left |
| M5.5 | Every parser is a library or is Rust | **done.** What this tree writes and what it links were both re-decided against measurement, and both moved: `demux_mp4` to Bento4, `demux_mkv` to libmatroska, `demux_flac`/`codec_flac` to libFLAC, `demux_mpa`/`codec_mpa` to libmpg123 -- and what was left, the parsers no library reads better, went to Rust: `codec_alac`, `codec_aac`, `demux_adts`. Every one of them bit-identical to the C++ it replaced, which is what made each move checkable rather than a judgement. [formats.md](formats.md) has every measurement, including the two upstream bugs a fuzzer found in Bento4 and the four things libmpg123's API did not say |
| M5.9 | The structural cut: `src/engine` and `src/player` | **done.** The portable half was one library holding both the audio engine and the thing that decides what to play. §4 answers yes to "could this ABI carry a DAW's engine", and a DAW taking it would have taken the transport, the playlist, the INI schema and the IPC wire format with it. They are `src/player` now, and `src/engine` has no route to them: the include path is what enforces it, so reaching across is a compile error rather than a review comment. CI builds `mediaperch_engine` alone, which checks both cuts at once |
| M5.75 | Path B is hashable, and a VST3 can be a stage in it | **done.** `mp::Processor` is `ProcessedGraph`'s arithmetic without the device, the ring or the threads, so `decode --path processed --gain --dsp` runs the chain and prints its SHA-256 -- the flags had been accepted and silently ignored, which is why nothing in this tree had ever compared the resampler between two builds. It found three bugs on the first run: `use_processed` could not see a gain, `Processor::reset` returned `MP_END` on success, and a seek left the noise shaper feeding back error from wherever the stream used to be. The baseline and AVX2 builds agree over 144 runs. `modules/dsp/vst3` hosts somebody else's plugin on `pluginterfaces` alone, with a VST3 written in `tests/` so the host is tested without one installed |
| M5.95 | ABI v3: several streams from one file | **done.** `select` named one stream and `seek(frame)` meant "the selected one", which has no answer once a player wants audio and video out of one file -- and appending would have left both meaning something narrower than their names. So `select_streams`, `seek(stream, frame)`, `MpPacket::reserved` becoming `stream`, and `stream_video_info` appended for the three colour code points §9.1 turns on. Checked against `demux_mp4` reading a real MP4 with two tracks in it, which is the first test here that drives a module rather than a fake. `demux_mkv` serves several tracks too, which is what makes v3 an interface rather than one module's habit -- and clearing MP_PACKET_TIMED on a video packet that never had a position is what that second container found |
| M5.97 | Section 9.9: a video packet says what its timestamp is counted in | **done.** MpVideoInfo::timescale, appended -- the first time the size prefix earned its keep, and a test asks for the older size to check it. Answering it turned up three more: MP4 was reporting decode timestamps where Matroska reports presentation ones, MP_PACKET_SYNC was claimed on every packet including video, and the edit list was read for audio tracks only -- sixty milliseconds of A/V offset in the fixture. A video seek landed one frame late, so it subtracts the track's largest composition offset before a lookup that indexes decode time -- with a guard for Bento4 reading that offset unsigned |
| M6 | Video: D3D11, DirectComposition, hardware decode, A/V sync off the audio clock | 4K HEVC plays with frames dropped against audio, never the reverse. **Started**: MP_KIND_VIDEO has a vtable, `video_d3d11` renders BGRA8 into a flip-model scRGB target or an off-screen one, and `read_back` makes the result a hash rather than a screenshot somebody looks at. §9's colour decisions are a separate testable header. NV12, P010 and the tone mappers wait for the decoder that produces frames for them |
| M7 | HDR: detection, scRGB present, the four tone-map providers, SDR white level | HDR content looks right on an SDR display *and* on an HDR display, and switching monitors mid-playback is handled |
| M8 | WinUI 3 shell | killing it mid-track changes nothing audible |
| M9 | Linux head | ALSA or PipeWire in an exclusive-equivalent mode, proving the core was actually portable |

M1 and M2 are the ones that de-risk the project. If exclusive-mode negotiation and the
module ABI both work, everything after them is ordinary work.

---

## 14. Findings to carry forward

Established from Microsoft's documentation during planning; each has cost other projects
real time.

- **`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` is normal, not an error.** On that return, call
  `GetBufferSize`, recompute the requested duration as
  `REFTIMES_PER_SEC / nSamplesPerSec * nFrames + 0.5`, and `Initialize` a second time on a
  *fresh* `IAudioClient` — the first one is spent. Any exclusive-mode implementation without
  this path is broken on some hardware and fine on the developer's.
- **Exclusive mode is a per-device user setting**, both "allow applications to take
  exclusive control" and "give exclusive mode applications priority". Either can be off.
  Handle the failure as a normal outcome with a clear message, never as a crash.
- **Session volume does nothing in exclusive mode.** Use `IAudioEndpointVolume`.
- **Exclusive mode silences every other application**, including system sounds. Microsoft's
  own guidance is to release the device when not in the foreground or not streaming, and
  MediaPerch will do that by default with an opt-out for people who want to keep the DAC.
- **`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` is required for the low-latency path**, and
  `Initialize` then allocates two buffers used ping-pong. Prefill the first before `Start`.
- **One Clang driver, two object formats.** The same `clang++` produces ELF on Linux and
  COFF on Windows, and the linker options are not the same words. `-Wl,-z,relro` is a
  hardening flag on one and three missing object files on the other -- `lld-link` reads it
  as an unknown argument followed by files called `relro`, `now` and `noexecstack`, and
  says exactly that. The fuzz job builds with the GNU driver on Windows and broke on
  precisely this. Hardening and `--gc-sections` are therefore asked once, in
  `cmake/CompilerOptions.cmake`, against the object format rather than the compiler.
- **`main`'s `argv` is in the process code page, and the ABI says UTF-8.** On a Japanese
  machine the two differ, and a file whose name is not ASCII reaches a decoder as bytes that
  name nothing — which surfaces as "no decoder recognised this file" and reads as a decoder
  bug rather than an encoding one. `CommandLineToArgvW` over `GetCommandLineW` is the only
  copy of the arguments that was never lossy. `SetConsoleOutputCP(CP_UTF8)` is the other
  half: without it the player cannot print the name of the file it just opened.
- **MMCSS: `AvSetMmThreadCharacteristics(L"Pro Audio")`**, reverted on stop. WASAPI itself
  applies `Pro Audio` to its transport threads below a 10 ms device period and `Audio` above
  it, so the numbers line up.
- **`IAudioClient3::GetSharedModeEnginePeriod`** gives shared-mode latency comparable to
  exclusive while keeping the mixer. It is the right fallback for people who want low
  latency but not silence from everything else — and worth offering before exclusive to
  users who only wanted the latency.
- **Tone mapping happens before composition, never during it** (§9.1). The *Stream HDR video*
  setting acts on frames that went through the video-processing stage; the DWM only clips. A
  renderer that decodes and presents its own texture gets tone mapping from neither, and
  that is the finding most likely to force a rewrite if it is discovered late.
- **The OS tone mapper maps PQ to gamma 2.4, not sRGB or BT.1886** (§9.2) — washed-out and
  black-crushed at the same time on a calibrated display. Good enough to default to, not
  good enough to call reference, and the reason `shader` exists as a provider at all.
- **`IDXGIOutput6` cannot see auto colour management** (§9.4). Reporting "SDR display" for
  both a plain panel and an ACM one is a correct read of a limited API, not a bug to hunt.
- **scRGB `1.0` means 80 nits on HDR and reference white on ACM-SDR** (§9.6). The same code
  is right on one and wrong on the other.
- **C23 is worth asking for and not worth relying on.** Measured on MSVC 19.51
  (`/std:clatest`): `static_assert` as a keyword, `typeof`, `[[attributes]]`, binary
  literals, digit separators and `()` meaning `(void)` all work. `bool`/`true`/`false` as
  keywords, `nullptr`, and **enums with a fixed underlying type** do not — and the last of
  those is the one an ABI header actually wants. clang has all of it. So: build C as C23,
  and keep `include/mediaperch/module.h` in the C11 common subset, because that header's
  whole job is to be read by a toolchain we do not control. `typedef uint32_t` plus untyped
  enumerators gives the same guaranteed field width everywhere.
- **What guarantees an ABI is `static_assert`, not the language version.** Every struct size
  and every member offset is asserted in the header itself, so the check fires in whichever
  language is compiling it — and `tests/abi_header_c.c` exists to make sure one of those
  languages is actually C.
- **CMake already knows MSVC spells both standards "latest".** `CXX_STANDARD 23` emits
  `/std:c++latest` and `C_STANDARD 23` emits `/std:clatest`; there is no `/std:c++23` and no
  `/std:c23` to ask for. Setting the flags by hand only earns a D9025 for overriding what
  CMake put there first.
- **The Ninja generator does not go looking for Visual Studio, and the failure is silent.**
  The VS generator locates the toolset itself; Ninja takes whatever `cc` and `c++` are on
  `PATH`. Outside a developer prompt that is MinGW GCC on a GitHub runner and Strawberry
  Perl's `gcc` or LLVM's `clang++` on a developer machine — and then everything builds,
  cleanly, and nothing in the log admits the compiler was not the one the preset is named
  after. Cost one CI run to notice. `MEDIAPERCH_EXPECT_TOOLSET`, set per preset and checked
  at the top of `CMakeLists.txt`, turns it into a configuration error that says what to do;
  the GCC rejection in `cmake/CompilerOptions.cmake` catches the same thing without a
  preset. Both exist because they catch different mistakes: `ninja-msvc` picking Clang is
  wrong even though Clang is supported.
- **The reference implementation was worth reading and not worth linking.** Writing an
  ALAC decoder from Apple's source produced a working, bit-exact decoder in one sitting and
  found five things the reference does not check on the way: `1 << (denshift - 1)` with a
  denshift the stream sets to zero, `x >> (32 - k)` with an unbounded k, a warm-up loop
  that writes `numactive` samples into a frame that may be shorter, a shift buffer ORed
  into the output without having been filled, and a four-byte read that runs past the
  packet on the last sample of every frame. Each of those is reachable from a file. That is
  what an unmaintained parser looks like from the inside, and it is a better argument
  against linking one than the CVE numbers are.
- **An OS decoder can be lossless and wrong at the same time.** Media Foundation
  returns multichannel ALAC in Apple's channel order and labels it with a WAVE channel
  mask. Every sample survives; not one of eight channels lands in its own speaker. Nothing
  in the output looks wrong, and the only reason it was caught is that the test put a
  different tone in every channel instead of the same signal everywhere. **A multichannel
  test whose channels are indistinguishable tests one channel eight times.**
- **Matching a GUID against the SDK constant matched nothing, silently.** `mfapi.h` builds
  `MFAudioFormat_ALAC` from the WAVE tag 0x6C61 over the standard media-subtype base;
  `IMFSourceReader` reports `{616C6163-767A-494D-B478-F29D25DC9037}` -- the four-character
  code 'alac' over the base Media Foundation uses for the codecs it gained in Windows 8.
  The check compiled, linked, ran, and was never true. It is the same shape of bug as the
  one it was written to catch, found the same way: by printing what was actually there
  rather than reasoning about what should have been.
- **The second compiler earned its keep again, on a two-line change.** Adding `u8` and
  `f64` to `SampleType` left two switches in `sine.cpp` non-exhaustive. MSVC said nothing;
  clang-cl produced `-Wswitch` for both. The enum values are now listed rather than
  defaulted, so the next type added to the ABI fails there until somebody decides what it
  means.
- **Media Foundation does not implement gapless metadata, in any codec.** Measured against
  FFmpeg reading the same files: MP3 starts 36.0 ms late, AAC 21.3 ms, Opus 13.5 ms, and
  each ends with padding the container said to discard. The control that turns this from
  four observations into one finding is raw ADTS, which carries no gapless information --
  there both decoders agree exactly. §11 will have to account for this before anything
  plays two tracks in a row.
- **A steady tone cannot measure a delay.** The first attempt at the MP3 alignment gave
  +3458 samples interleaved and -1151 per channel, from the same files: a sine correlates
  with itself once per period, so every peak is a plausible answer. Pink noise gave +1729
  frames unambiguously. **A test signal that cannot distinguish the answers is not a
  measurement**, and it looked exactly like one.
- **Media Foundation clips float WAV**, converting it to 32-bit integer and pinning
  everything above unity -- 73.8% of the samples in the test file. A *lossless* format,
  altered, by the decoder that scores lowest on it for reasons written down before this was
  measured. The scoring turns out to have been right for a better reason than the one
  given.
- **"It is lossy, so anything will do" is a conclusion, not a premise.** Vorbis and Opus
  have no bit-exactness to protect, which is a good reason to ask whether four submodules
  are worth it -- and a bad reason to assume the answer. Measuring found that Media
  Foundation cannot open Ogg at all, and that where it can decode these codecs it emits
  13.5 ms of wrong audio at the start of every Opus track. The premise was right and the
  conclusion would have been wrong.
- **A default of 16 bits is a decision, and it was the wrong one.** `decode_mf` asked for
  16-bit output whenever a stream declared no depth of its own -- which every compressed
  stream does. Every lossy codec here decodes to float, so that was a quantisation
  performed inside our own decoder, invisibly, on a signal that had more in it. Asking for
  32 gets 32 wherever the decoder can produce it.
- **A dependency that asks git for its own version number is a dependency that fails on
  somebody else's machine.** opus derives its version from `git describe`
  and commit no fallback, so a CI runner fetching submodules at depth 1 gets version `0`
  and opusfile turns that into a hard configure error. It passed locally for the least
  interesting reason available: a full clone has tags. **A build that reads the repository
  is a build whose result depends on how the repository was obtained**, and the fix was to
  stop asking -- the versions are pinned beside the gitlinks now.
- **Seeking had never been tested, in any decoder.** There was no way to ask for it from
  the command line, so there was no way to check it, so nobody had. `--seek` makes it
  falsifiable in one line: the hash of a decode seeking to frame N must equal the hash of
  the last (length - N) frames of a straight decode. A capability with no way to observe it
  is a capability nobody knows the state of.
- **Ranking without a fallback makes every refusal fatal.** Probing sees four kilobytes;
  opening sees the file. Once two decoders had good reasons to refuse a file they had
  scored highest on, "pick the best" had to become "pick the best that opens" -- otherwise
  correcting `decode_mf` turned a wrongly-decoded file into an unplayable one.
- **A whole-image flag applied to part of the image fails twice, and the second time was
  avoidable.** `/guard:ehcont` was restricted to C++ on the reasoning that C has no
  exceptions. MSVC emits compound EH metadata for C objects too, so `/CETCOMPAT` rejected
  every object of libFLAC -- a pure C library -- with `LNK2047`, exactly as it had rejected
  every object of Catch2 when the flag was target-scoped instead of directory-scoped. The
  first time cost a link; the second cost another, because the fix was reasoned about rather
  than built. A build was available both times.
- **A decoder can open a file, describe it correctly, and produce nothing.** `dr_flac`
  opens a 32-bit FLAC, reports 32 bits from STREAMINFO, and decodes zero frames: its
  frame-header table still marks the bit-depth code FLAC 1.4 assigned to 32 bits as
  reserved, and `DRFLAC_ASSERT(bitsPerSample <= 24)` runs through its decode paths. Nothing
  returns an error. For this program that is the worst failure available -- `read` returns 0,
  the graph reads that as the end of the stream, and the track is skipped in silence. So
  `Decoder::open` decodes one frame and rewinds before declaring success. The check costs one
  frame and guards every decoder, including the ones not written yet.
- **A decoder's random number generator is part of its state, and `init()` was not
  resetting it.** AAC fills noise-substituted bands from a generator carried in the decoder.
  `Decoder::open` decodes one frame to prove the decoder works and then seeks back, and that
  check is skipped when the length is unknown -- so **raw ADTS decoded perfectly and the same
  bitstream in an MP4 decoded at 8 dB**, because only the MP4 knows its own length. The
  verification step changed the thing it was verifying. Every band was the right width and
  carried the right energy and held different noise, which is the shape of error that a
  length column and an SNR summary both describe as "wrong somewhere". `tests/aac_test.cpp`
  now decodes a frame, calls `init()`, decodes it again and requires the same samples.
- **A test file that does not exercise the feature reports success for it.** Every PNS
  experiment ran against an 8 kHz reference that turned out to contain **no noise-substituted
  bands at all**, so each change came back "no difference" and each was read as "no problem".
  The tell was arriving at it backwards: inverting the sign of the generated noise changed
  the output by 0.00 dB, which is impossible if any of it is being used. **A control that
  cannot fail is not a control.** Encoding with `-aac_pns 0` and watching the disagreement
  vanish is what turned a week of plausible theories into one file to look at.
- **A reference decoder's internals can be read from its output.** FFmpeg's noise for a band
  is not visible from outside, but MDCT analysis of a *reconstructed* signal returns the
  coefficients that were synthesised -- so applying the forward transform to FFmpeg's PCM
  recovered its spectral coefficients exactly, and they could be compared against the
  generator's sequence directly. The method was validated on our own output first, where the
  expected values were already known. **When the question is "what did the other
  implementation compute", inverting its output beats reading its source.**
- **Every channel perfect and two of them in the wrong speakers, again.** FFmpeg writes
  `channel_configuration = 0` for 7.1(wide) and puts the layout in a program config element,
  whose front elements are ordered **from the centre outwards** -- so the first pair after
  the centre is front-left/right-*of-centre* and the second is the main left and right.
  Reading them in the obvious order gave -0.09 dB overall while every one of the eight
  channels matched *some* FFmpeg channel at 136 dB. The per-channel correlation matrix is
  what made that legible; the overall figure said only "wrong". Same class of bug as Media
  Foundation's ALAC channel order, found deliberately this time rather than by luck.
- **Two constants and a table type, worth 45 dB between them.** The TNS filter's quantiser
  step folded in `coef_compress`, which does not belong there -- fixing it took one file
  from 101.5 dB to 135.8. The IMDCT cosine table was `float`, which cost 12 dB against the
  same table in `double`; a lookup table is the last place to save memory, because every
  output sample sums a thousand of its entries. And full scale was found by fitting the
  ratio to FFmpeg's output -- 3.05175748667e-05, whose base-2 logarithm is 15.000000 -- which
  is a legitimate way to recover a constant the specification states in units the code did
  not use.
- **The identity that checks a window found the bug the ear could not.** A sine window
  written as `sin(pi/N * (2i+1))` instead of `sin(pi/N * (i+0.5))` decodes to something that
  sounds like music. The Princen-Bradley condition -- `w[i]^2 + w[N/2-1-i]^2 == 1` -- gave
  0.000005, 0.184 and 2.0 where it must give 1, in three lines of test code. It stayed hidden
  as long as it did because the file being used happened to select the KBD window, which is a
  different code path. **Check the mathematical property, not the audible result.**

- **"Compare it with the original file" is a different measurement, not a better
  one.** The obvious way to prove a lossy decoder is right is to hold it against
  the uncompressed audio that was encoded, and the obvious expectation is that
  this would show which of two decoders is closer. It cannot: at 256 kbps the
  decode sits 17 dB from the source and two correct decoders sit 134 dB from each
  other, so the encoder's loss is common to both and six orders of magnitude
  larger than anything that separates them. What the source *can* do is
  everything a decoder-to-decoder comparison cannot -- length, alignment, which
  channel came out of which speaker, per-band energy, and a floor that assumes
  nothing about FFmpeg. Both are now in CI, and the reason both are is the next
  entry.
- **The three bugs of this milestone were put back one at a time, and one of them
  walked through every source-referenced check.** Reading the program config
  element outwards-in was caught on channel order; ignoring the edit list was
  caught on alignment, in seven rows of twelve. Failing to reset the noise
  generator passed length, alignment, channel order, band energy and the fidelity
  floor -- because a noise-substituted band is arbitrary by design, so the wrong
  noise at the right energy in the right band is invisible to the source and only
  visible against the other decoder, which caught it by a hundred decibels.
  **A test that has never failed is a test nobody has reason to believe**, and
  deliberately breaking the decoder is the cheapest way to find out which checks
  were doing work.
- **The instrument had a bug the unit tests found before any file did.** The
  alignment search correlated channel 0 with channel 0 -- so when the channels
  were permuted, the two were unrelated at every lag, the search settled on
  noise, and *everything measured afterwards*, including the channel matrix that
  would have reported the permutation, was measured at a meaningless offset. A
  sum over channels is invariant under permutation, which is exactly the property
  wanted, and the search runs on that now. The same tests found the band check
  reporting a 9 dB disagreement in a band holding a millionth of the energy,
  where what it was measuring was the analysis window's own leakage. **Code that
  decides whether other code passes has to be tested harder than the code it
  judges**, because when it is wrong it is wrong in the direction of saying
  nothing.

- **A linker keeps what is *referenced*, not what is reachable, and 48 KB of a
  decoder turned out to be an encoder.** `mp_decode_ogg.dll` is the largest
  thing this project ships, and the largest single object inside it is
  libvorbis's `psy.obj` -- the psychoacoustic model, which only an encoder uses.
  It is there because `_vds_shared_init` is one function serving both directions
  and calls `_vp_psy_init` inside `if(encp)`: the branch never runs in a decoder,
  the *reference* is unconditional, and the linker cannot tell the difference. It
  brings `tonemasks` with it, 22 KB of tables that nothing will ever read.
  Removing it means patching a submodule, so it stays -- but it is measured and
  written down rather than assumed to be "libvorbis being big".
- **A size number without an attribution is not a measurement.** `tools/mapsize.py`
  reads a linker map and charges every byte to the object that brought it, which
  is what turned "the Ogg module is 365 KB" into the finding above in about a
  minute. Writing it was three false starts, each of which is a lesson in
  measuring: a regex that assumed a fixed column width attributed a third of the
  binary to a symbol called `i`; taking address deltas across section boundaries
  charged the alignment padding to whichever function happened to be last; and
  counting `.bss` -- which occupies image space and no file space -- put 188 KB
  of zeroed lookup tables against a 78 KB DLL. **A tool that reports plausible
  numbers is more dangerous than one that crashes**, and the only reason these
  were caught is that the totals were checked against the size on disk.
- **"Simple" in C++ means "does not pull anything in", not "short to write".**
  `std::to_string` on an `unsigned` reaches the same shortest-round-trip float
  machinery `std::format` does -- Ryu's tables and the locale facets around them
  -- because it has to be ready for a `double`. `std::ofstream` brings iostreams,
  a static initialiser and the locale facets again. Neither reads as expensive at
  the call site, and between them they were 8.5 KB of a 123 KB executable whose
  entire use for them was printing a sample rate and writing bytes to a file.
  `std::to_chars` and `std::fopen` do the same work with no tables at all.
- **The measuring apparatus was 27% of the probe.** `compare` and `verify` are
  how the hard bugs here were found and are not needed to play a file; leaving
  them out of an optimised build takes it from 169.5 KB to 123.5. The libraries
  are still compiled and still unit-tested in every configuration -- they are in
  static libraries, so nothing links them once nothing calls them, and the code
  cannot rot from disuse. Asking a build without them for one exits 77 rather
  than failing, so a test runner can tell "this build cannot answer that" from
  "the answer was wrong".

- **A vendored library's sanity ceiling reads as our refusal.** `dr_wav` rejects any file
  above `DRWAV_MAX_SAMPLE_RATE`, which defaults to 384000 — its own guard against garbage
  headers, not a WAV limit, since the field is 32 bits wide. A 768 kHz file therefore came
  back as "unsupported by this module", which points at the wrong culprit entirely. Two
  things came out of it: the constant is now raised to 6,144,000, and the module logs *which
  library declined* at debug level, because "we could not open it" and "dr_wav would not open
  it" are different sentences and only one of them is actionable.
- **Neither decoder reads everything.** Measured at the edges: `decode_mf` handles 32-bit WAV
  at 1,048,575 Hz — the FLAC spec's ceiling — without complaint, and refuses FLAC above about
  655 kHz. `decode_native` is the other way round. The two cover each other exactly, which is
  the clearest argument the module architecture has produced so far. [formats.md](formats.md)
  has the table.
- **Media Foundation is bit-exact for WAV and FLAC.** This was not safe to assume. A source
  reader will insert a converter to produce whatever media type it is asked for, and the
  conversion is invisible — the samples simply come back different — so `decode_mf` reads the
  *native* media type first, asks for PCM at exactly that depth, and then reads back what the
  reader actually agreed to rather than reporting what it asked for. Measured across 16- and
  24-bit WAV and FLAC: **every hash equals `decode_native`'s and FFmpeg's**. The OS decoder is
  a real option, not a fallback to apologise for.

  One difference worth knowing, and it is metadata rather than samples: for FLAC, Media
  Foundation reports a channel mask (`0x3` for stereo) where `dr_flac` reports none, because
  FLAC has no channel-mask field and the two libraries disagree about whether to supply the
  conventional one. Both are defensible. It changes the first candidate offered to a device
  and nothing else, because §6.1 offers the extensible form either way.
- **A virtual cable is not a transparent loopback, and it fails silently.** The obvious way
  to prove bit-exactness past our own code is to play into a virtual cable's render endpoint
  and record from its capture endpoint. Measured with both endpoints taken in *exclusive*
  mode at the cable's own configured format (192000 Hz, 24-bit packed), both endpoint volumes
  at exactly 1.0000, and zero reported discontinuities: the recording **correlates** with what
  was played — 0.87 at 44100/16, 0.51 at 192000/24 — and is **never identical**. Samples come
  back repeated in runs and with their low bits cleared. No prefix of the played stream, down
  to six bytes, appears verbatim anywhere in the recording.

  The lesson is about what exclusive mode promises. It guarantees that *Windows* does not
  touch the samples: no mixer, no APO, no resampler, no volume. It cannot guarantee what a
  **driver** does, and for a virtual device the "hardware" the driver hands the buffer to is
  more software. So the loopback is not an instrument that can measure this, and a failure
  there is a statement about the cable rather than about the player. `verify --loopback`
  reports it in those words rather than as a red result.

  What can be proved is everything up to `ReleaseBuffer`, and that is proved: the tee in
  `verify` copies every committed buffer and hashes it. Measured on a real device, 16-bit and
  24-bit, 44100 and 192000: the SHA-256 of the FLAC's decoded PCM, of FFmpeg's decode of the
  same file, and of the bytes handed to the device are all one hash. Going further needs
  external instrumentation — a digital output recorded by a second interface — not more code.
- **Wrapping a vtable means wrapping all of it.** The tee copies an `MpSinkVtbl` and replaces
  the entries it cares about. Leaving the rest pointing at the real module is not a harmless
  pass-through: the *handle* is the wrapper, so the module reads its own struct out of a
  `TeeSink` and the process dies at the first `negotiate`. An access violation is a cheap way
  to learn it; a subtly wrong `get_position` would not have been.
- **`ENDPOINT_HARDWARE_SUPPORT_VOLUME` is a weaker claim than its name.** It means the
  volume control is not the Windows engine's. It says nothing about whether the driver
  applies it by scaling samples or the hardware applies it after the converter — and every
  endpoint on this machine claims it, including a VB-Audio virtual cable, which has no
  hardware to apply anything with. Report it as "the endpoint has a volume control", never as
  "this volume is free".
- **Two devices, opposite channel-mask requirements.** The virtual cable refuses the plain
  `WAVEFORMATEX` at every width and takes only the extensible form; the FiiO KA5 takes the
  plain form at every width and never needs a mask. There is no order of trying them that is
  right for both, which is the whole argument for offering each container in both forms
  before moving to the next container. See [devices.md](devices.md).
- **The shared-mode dropdown is a setting, not a capability list.** The KA5's is 384000 Hz;
  exclusive mode accepted 705600 and 768000. The virtual cable's is 192000/24; exclusive mode
  accepted 44100/16. Reading it tells you what the engine is configured for and nothing about
  what the driver will take -- except for one thing that turned out to matter more than the
  rest of it, which is `nBlockAlign`, because that is what revealed that "24-bit" meant three
  bytes.
- **"24-bit" is two containers, and devices disagree about which one they mean.** Measured
  on this machine: a VB-Audio virtual cable configured for 24-bit reported
  `nBlockAlign = 6` for stereo — three bytes per sample, `S24_PACKED` — while the onboard
  Realtek codec reported `nBlockAlign = 8` with `wValidBitsPerSample = 24`, which is 24 bits
  inside four. A candidate list that offers only one of them refuses playable audio on half
  the hardware, and the failure looks like a device limitation rather than a missing case:
  before the fix, `negotiate --bits 24` on the cable reported all four candidates refused and
  it was easy to believe the driver. After it, candidate 4 — `S24_PACKED` with a channel mask
  — is accepted at 44100 and at 192000, at the minimum period of 2.00 ms. This is why
  candidates are generated over containers and why the transform is called `repack` rather
  than `promote`.
- **A real device refused the plain `WAVEFORMATEX` and took only the extensible form.**
  Measured on a VB-Audio virtual cable at 44100/16/2: candidate 1, the plain form, came
  back `AUDCLNT_E_UNSUPPORTED_FORMAT`; candidate 2, the same format as
  `WAVEFORMATEXTENSIBLE` with `dwChannelMask = 0x3`, was accepted, at the minimum period of
  88 frames (2.00 ms). This is the case §6.1 was reordered for. Had the mask variant been
  appended after every other container -- which is how the rule reads written out as prose --
  this device would have been offered the three- and four-byte containers first, and the
  stream would have been repacked needlessly while still reporting itself bit-exact. The
  reordering was a guess when it was made and is a measurement now.
- **Reporting a thread's state from the thread that started it is a race, and it lies
  convincingly.** `PassthroughGraph::start` launches the render thread and returns; the
  caller then read `hooks.realtime()` and printed "MMCSS REFUSED", which sent an afternoon
  after a service that was running the whole time and a `Pro Audio` profile that was
  registered the whole time. `AvSetMmThreadCharacteristicsW` had simply not been called yet.
  Anything a worker thread discovers is published through an atomic with a `pending` state,
  and `pending` is displayed as "has not answered yet" rather than folded into "no".
- **The second compiler earns its place immediately.** clang-cl rejected a default argument
  of a nested type whose default member initializers were needed while the enclosing class
  was still incomplete; MSVC compiled it without a word. Clang is right, and the fix was to
  move the type to namespace scope. One instance is not a policy, but it is the first thing
  the `clang` CI job found, on the first tree it was pointed at.
- **`/CETCOMPAT` and `/guard:ehcont` are whole-image flags and must not be target-scoped.**
  The linker requires that *every* object carrying C++ EH metadata was compiled with
  `/guard:ehcont`, third-party code built in-tree included — and Catch2 never sees an
  interface library it does not link. Target-scoped, they produce `LNK2047` on every Catch2
  object, which reads like a Catch2 problem and is not one. They live in an
  `add_compile_options` at directory scope, applied before any subdirectory is added, and
  `/guard:ehcont` drags `/guard:cf` along with it. The MSBuild and Ninja generators do not
  agree about when this is fatal, so a green Ninja build is not evidence.

---

## 15. Risks

| Risk | Mitigation |
|---|---|
| Driver-specific exclusive-mode behaviour that no amount of reading predicts | the device matrix in §12, and treat every negotiation failure as a first-class outcome rather than an assertion |
| The module ABI ossifies too early and every change becomes a break | `size`-prefixed structs (§4.2) and a v1 that is deliberately small. Do not add an interface until the second implementation of it exists |
| Memory-safety bugs in parsers | ~~now that Rust is not doing that job~~ — it is, for the three parsers this tree still writes; see §2's *When to revisit*. The rest of the mitigation stands and does the heavier lifting, because most parsing bytes are somebody else's library: libFuzzer on every parser from M2, ASan/UBSan in CI, `/GS` and `/guard:cf` in release, and `demux_ffmpeg` out of process. **That last one is done, by a route the plan did not name**: the module drives the `ffmpeg` command line rather than linking libavformat, so FFmpeg's parsing surface is already in a process that can die without taking the audio with it, and `mp_host_ffmpeg.exe` (§4) is a thing to build only if a module ever needs to be linked in |
| The video half quietly becomes the whole project | audio is complete and shippable at M5. Video is M6 onward and is allowed to be late |
| FFmpeg's licence and binary size make it awkward to ship | it is a module, so ship it separately. The base install still plays music without it -- nine demuxers and eight codecs of this tree's own -- and will play video without it too, because §9.8 puts hardware decode on an `IMFTransform` rather than on anything FFmpeg links |

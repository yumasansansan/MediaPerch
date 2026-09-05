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

### ABI v4: a frame describes its pixels rather than naming them

**Done.** `MP_ABI_VERSION` is 4, `MpPixelFormat` is gone, and `MpPixelLayout` is what a frame
and `read_back` carry.

v3 named six formats -- NV12, P010, BGRA8 and three render targets -- and every one of those
names is DXGI's, sitting in the one header meant to outlive Direct3D. It could not say 4:2:2,
4:4:4, 4:0:0 or twelve bits **at all**, which mattered the moment the question was asked out
loud: H.265's range extensions reach sixteen bits and 4:4:4, dav1d produces 4:2:0, 4:2:2 and
4:4:4 at eight, ten and twelve, and openh264 produces exactly one of those and refuses the
rest. An ABI that cannot name a format is an ABI that truncates it, and truncation was
precisely what this had to be checked for.

Naming the combinations instead was the alternative, and it is not one: five chroma layouts
by five depths by three packings is **seventy-five enumerators** before alpha and float, and
a switch of that size is a switch nobody writes correctly twice.

So six fields -- chroma, packing, significant bits, container bits, shift, flags -- and
everything else is arithmetic over them. How many planes, how large a chroma plane is, how
many bytes a pixel takes, and what a sample must be scaled by are `mp_pixel_*` inline
functions in the header, with their formulae in the comments beside them so a module in a
language that cannot see the functions derives the same numbers rather than guessing.

**`shift` is the field that earned the break**, and it is not a tidiness argument. Ten bits
in a sixteen-bit container is not one thing: P010 puts them at the top and dav1d hands them
back at the bottom. Same depth, same container, same chroma -- and a consumer that assumes
either is wrong about the other **by a factor of sixty-four, as brightness rather than as an
error**. v3 chose between them with a `bool ten_bit`, which had no way to be right about
both. The general formula is

    scale = (2^container_bits - 1) / ((2^bits - 1) << shift)

and P010 falls out of it as `65535 / (1023 * 64)` -- character for character the constant
`yuv_matrix.cpp` carried before, which is how a refactor gets to be a refactor rather than a
rewrite. `tests/pixel_layout_test.cpp` holds it there, along with twelve, fourteen and
sixteen bits, and it needs no GPU, no decoder and no file.

Two things fell out of doing it now rather than after a decoder that produces 4:4:4:

- **A latent bug in `codec_mft`.** Its system-memory path labelled every frame NV12 while
  `set_output_type` accepts NV12 *or* P010 -- a ten-bit picture reported as eight. The
  subtype was already being read and thrown away; it is kept now.
- **4:2:2 and 4:4:4 semi-planar cost nothing.** The presenter's plane sizes come out of
  `mp_pixel_chroma_width` and the shader samples with normalised coordinates, so writing the
  general form was *less* code than writing the 4:2:0 special case. Planar is still refused,
  with a sentence, because three planes want a third sampler read and no decoder here
  produces them yet.

This was the cheapest moment for it: one producer (`codec_mft`) and one consumer
(`video_d3d11`). After `codec_dav1d` it would have meant writing a three-plane path in the
old vocabulary and then rewriting it.

#### And then the presenter took the shapes v4 could describe

**Done, in the same shape the ABI took.** `video_d3d11` renders 4:0:0, 4:2:0, 4:2:2 and
4:4:4, planar or semi-planar, at eight through sixteen bits. Interleaved Y'CbCr -- YUY2,
AYUV, Y410 -- is refused with a sentence: it is a different unpack and nothing here produces
it.

The shape of the code is the argument for v4 restated. There is **one** matrix and **one**
transfer in the shader, in a `convert` a two-plane and a three-plane entry point both reach,
so a 4:2:0 frame and a 4:4:4 frame are held to the same arithmetic by construction rather
than by two functions being kept in step. And the subsamplings need no case at all: chroma is
sampled with normalised coordinates, so a half-width plane and a full-width one are the same
call. Writing the general form was **less** code than the 4:2:0 special case it replaced.

Two things were worth finding out by writing the tests rather than by reasoning:

- **4:0:0 fails as a picture, not as an error.** A monochrome frame has one plane; sampling
  two textures that were never made returns zero, and zero through `(c - 0.5) * chroma_scale`
  is a strong green. So the shader is told there is no chroma -- a `has_chroma` of 0 that
  multiplies the *centred* chroma -- rather than left to read what is not there.
- **Neutral chroma is 127.5, not 128.** Half of the full scale falls between two codes at
  every even depth. The first version of the monochrome test expected `bt709(y, 128, 128)`
  and disagreed with the shader by a ten-thousandth of the range; the shader was right.
  Saying "no chroma" means saying exactly 0.5, which no integer code can.

Ten and twelve bits are tested **at the bottom of a sixteen-bit container**, which is the
case that did not exist before v4 and is what dav1d will hand over. P010's ten bits at the
top are the other, and both go through the same `mp_pixel_sample_scale`.

Planar has no producer in this tree yet, and the frames in `tests/video_d3d11_test.cpp` are
built by hand -- which is exactly how the presenter was checked before there was anything to
decode at all.

#### A version bump is global, and the Rust mirror did not hear it

**Five modules were dead for a day, and three checks each had a reason not to say so.**

`modules/shared/mp-abi` is the Rust mirror of the header, and it carries no video structure
at all -- the Rust modules are AAC, ALAC and DSD decoding and the ADTS and DSD demuxers. So
v4, which changed the video half and nothing else, left nothing in that file to update and
produced no compile error. Its `ABI_VERSION` stayed at 3 while the header went to 4, and
`mp_module_entry` answers null to a host on another version, which is the whole point of a
bump. **AAC and ALAC quietly became FFmpeg's, and DSD lost its bit-exact DoP path and came
out as F32** -- Path A silently became Path B for every DSD file.

The interesting part is why nothing caught it, because each reason is a check working as
designed:

- **The C++ tests load the modules they name**, and no test names those five. The Rust
  modules had never needed one, because they had never been the thing under test.
- **`rust_modules` builds the crates and runs their own tests**, which know about the format
  they parse and nothing about a host.
- **`format_matrix` would have shown it in one line** -- and it needs FFmpeg and a corpus, so
  it skips in every CI leg that builds. It passes in 0.03 seconds there and takes 24 on a
  machine that has them. The one job with FFmpeg ran `decode_quality` alone, by name. So the
  table that would have caught this was only ever really measured by a person. That step
  selects on the `quality` label now, which is what both checks already carried and what a
  third one would carry too.

`tests/module_abi_test.cpp` is what catches it now, and it deliberately names nothing: it
walks the directory the modules are built into -- 34 DLLs -- and requires each to answer at
MP_ABI_VERSION, to report that version back, and to refuse the versions either side. The
module that falls behind is the one nobody remembered to name.

It **collects** rather than asserting where it finds, which is the difference between one
run and five: a REQUIRE inside the loop stops at the first module and says nothing about the
rest. Put back to 3 deliberately, the sweep names all five in one failure --

    modules that answered null at MP_ABI_VERSION 4:
      mp_codec_aac.dll
      mp_codec_alac.dll
      mp_codec_dsd.dll
      mp_demux_adts.dll
      mp_demux_dsd.dll

-- which is the measurement that says the check works, rather than the hope that it does.

Two smaller checks broke the same way and are worth recording together, because the shape is
the same -- a check that stops checking without going red:

- **`clang_syntax` had its include paths listed by hand.** Three shared modules became five
  while nobody edited `cmake/ClangSyntax.cmake`, and the second front end started failing on
  a missing header rather than on a diagnostic. It globs `modules/shared/*` now, which is the
  list it was trying to be.
- **Microsoft's H.264 transform can answer a drain before it has finished.** One CI leg of
  six lost the last frame of twenty-four, with no code change from the green run before it,
  on a machine taking eight times as long per test; forty runs here never reproduced it. The
  ABI says `next_frame` drains until MP_END after a flush, so relaying an early
  NEED_MORE_INPUT would make this module break its own contract because somebody else's
  decoder broke theirs -- and the frame it costs is the last one of every file. `codec_mft`
  now doubts the end of a drain four times, a millisecond apart, before believing it.

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
presentation time everything else follows. `MpSinkVtbl::get_position` is that pair in the
ABI, and both sinks have answered it since they were written.

The consequence has to be stated, because it is the opposite of the usual answer: **in
exclusive passthrough there is no resampler, so audio cannot be rate-matched to video.**
Video therefore drops or duplicates frames against the audio clock, always. Never adjust
audio to keep video smooth — that is a conversion, and this player does not do conversions
it did not announce.

**Done, as arithmetic that reads no clock.** `src/engine/mediaperch/avsync.hpp` is `AvClock`
and `VideoPacer`, and neither of them calls `QueryPerformanceCounter`, touches a sink, or
knows what Windows is: a reading and a tick go in, a decision comes out. That is what makes
§8 testable without a sound card — every question it asks is a question about a device
running fast, or slow, or stopped, and none of those can be arranged on real hardware — and
it is the same file on the Linux head, where the ticks come from `CLOCK_MONOTONIC`.

**The rule that audio never moves is enforced by absence.** There is no method on either
class that adjusts a sink, and `AvClock` has no way to ask one for anything. A rule that
cannot be expressed cannot be broken by accident.

#### What is audible is not what the device was given

Three separate offsets sit between "the file's frame N" and "what a listener hears now", and
each one had been recorded somewhere in this tree without anything reading it:

- **The device buffer.** `position_frames()` counts what the render thread *committed*, which
  is up to one device buffer ahead of what has played. That is deliberate and design.md's
  resume argument is where it is settled: the number exists so a run interrupted by a device
  being pulled out can come back, and the device that could say what it actually played is
  the one that just vanished. Measured here at 30 ms with a 10 ms period and a three-period
  ring — a frame and a half at 24 fps, in the direction that shows video early.
- **The chain.** What leaves the endpoint went through the DSP chain, so it came from source
  material `latency_frames` earlier. `MpDspVtbl::get_latency` was appended for exactly this
  and §5 said so at the time; `AvClock` is the first thing to subtract it.
- **The anchor, which a seek moves.** A seek does not stop the device: audio already
  committed still plays, so the frame seeked to becomes audible when the device reaches what
  had been *written* at that moment, not what it had played. The graphs have kept both
  numbers — `played_base_` and `rendered_base_` — since gapless was written, and this is the
  first code to read them against the device rather than against the write counter.

`clock_spec()` on either graph states all of it, because a graph is the one thing that knows
all of it: it owns the sink, it knows what the file counts in and what the device counts in,
it built the chain, and it holds the anchor.

#### Two thresholds, and no third

A frame is **shown** once the clock has reached its timestamp, and **dropped** once the clock
has passed it by more than one frame interval — at which point the next frame is already due
and showing this one would only make that one later too. Everything else is **repeat**, which
is not an instruction to duplicate anything: a display that is not given a new frame shows
the old one, so duplication is what happens when nothing is done. There is no third rule and
no tuning knob, because a third rule is where a player starts guessing.

The interval comes from `fps_num/fps_den` when the container states it — a ratio, because
rounding 24000/1001 is how a player drifts a frame every seventeen minutes — and is measured
from consecutive timestamps when it does not, which is normal for a container that timestamps
every frame. **Until two timestamps have been seen, nothing is dropped at all**: a late frame
shown is a blemish, and a frame dropped because the rate was guessed is gone.

Asking about the same frame twice is safe and moves no state, which is what makes `repeat`
usable from a polling loop.

#### The skew nobody would have found twice

§9.9 measured sixty milliseconds between the two timelines in this tree's own fixture: the
engine applies the audio track's edit before the source counts a frame, and video timestamps
stay container-relative. `VideoPacer::set_skew_seconds` is where whoever knows both edits
subtracts them, once, rather than each side folding it in differently.

#### Nothing accumulates, so nothing drifts

Every answer is computed from the latest reading rather than from a running total, so a bad
reading is wrong once and gone by the next one. That is also the argument for doing the
comparison in `double`: §9.9's objection to nanoseconds is about *rounding every timestamp
into a unit that cannot hold it*, which nothing here does — the timestamps stay in the
container's own integer ticks and one division happens at the point of comparison, on numbers
whose difference is a few milliseconds. The test runs an hour of a clock read once a second
and checks the last answer is exact.

#### Checked against a real stream

`tests/avsync_test.cpp` decodes `av1.mp4` and paces the frames it gets back, because the
order matters and is not the order the packets arrive in: an MP4 stores samples in decode
order and this fixture has B-frames, so the decoder is what puts them in presentation order.
Reading the packets and sorting them would have produced the same list and proved nothing
about who does the sorting.

Three clocks, one file. At the right speed, all 24 frames are shown, none dropped, and the
run takes as long as the file is. Half a second ahead, twelve frames go and twelve are shown
— and the audio is untouched, which is the whole of §8. Stopped, a thousand polls produce a
thousand repeats, nothing shown and nothing dropped: the picture holds.

#### The graph that applies it

`src/engine/mediaperch/video.hpp` is the video half of a player: `VideoDecoder` and
`Presenter` behind their vtables — `mp::Sink` for pictures — and `VideoGraph`, which decodes,
asks the pacer, and presents.

**One frame in hand, and no queue.** A decoded frame is valid until the next call on the
codec that produced it, which the ABI says because a hardware decoder hands out a slice of a
pool it owns. A graph that queued frames ahead would have to copy them, which is precisely
what §9.8.1's texture-adoption argument exists to avoid. So the graph holds one frame,
decides about it, and asks for the next only when it is done — and the lookahead a queue
would have bought is already inside the decoder, which reorders B-frames and, since M6.6,
runs on every core the machine has.

**It owns no thread**, which is the difference from the audio graphs. They own one because
the device's own event is what paces them. Video's pace is the display's, and a display
belongs to the head — DirectComposition, a swap chain's waitable object, a vblank. So `pump`
is a call, and whoever owns the display loop makes it.

**A drop does not cost a refresh.** One `pump` lets go of as many past frames as it has to
before it finds one to show. A decoder that fell behind has several frames whose time has
passed, and letting one go per display refresh would never catch the clock — the thing that
looks like a stall and is actually a policy.

**Where the container and the bitstream disagree, the bitstream wins.** After the first
frame the graph asks the decoder what it actually produced and reconfigures the presenter if
the answer differs — geometry, primaries, transfer, matrix, range. `codec_mft` needs this:
its geometry comes out of the sequence parameter set and not out of the container. The
exceptions are the timescale and the frame rate, which are never taken from a decoder,
because a decoder does not re-time a stream — the codecs report zero there and say so.

**Packets come from an interface, not from a demuxer**, and that is a deliberate hole with a
name. §4 says one file has one position: audio and video out of the same file must come from
one demuxer with both streams selected, and the thing that reads it once and routes what
comes out does not exist yet. `IPacketFeed` is what it will implement. A caller reading a
single stream implements it in four lines today, and nothing above it has to change when the
router arrives.

`tests/video_graph_test.cpp` runs it: `demux_mp4` reading `av1.mp4`, `codec_dav1d` decoding
it with the presenter's own device handed over, `video_d3d11` on WARP, and a clock that is a
number. At the right speed, 24 decoded, 24 shown, none dropped, nothing more than a
millisecond late, and the run takes as long as the file is — a pump that presented on demand
would have finished in microseconds. Half a second ahead, twelve go and twelve are shown, and
**the picture at the end is still right**, because dropping a frame is not skipping a decode:
every frame was decoded, since a frame nobody decodes is one the next frame references.
Stopped, five hundred polls and the picture holds.

#### One demuxer, and the position two consumers share

**Done.** `PacketRouter` in `src/engine/mediaperch/packet.hpp` reads one demuxer once and
hands each selected stream an `IPacketFeed`. The hole `VideoGraph` was written around is
filled, and nothing in `VideoGraph` changed: it took the interface for this.

The ABI header says what is wrong with the alternative in as many words — opening the
container twice means two file positions, and a seek then has to move both and land them on
the same moment. `PacketRouter::seek` is one call for exactly that reason: it moves the file
and empties every queue, because those two halves cannot be separated. A seek that left the
queues alone would hand a consumer packets from before it, which is the one thing a seek is
against.

**The queues are the only buffering, and most packets miss them.** A consumer asking for its
own stream has the demuxer read **straight into its own buffer**; only a packet that turns
out to belong to somebody else is moved aside, and only until they ask. Serving a queued
packet is a vector swap rather than a copy, and the vectors are recycled, so a 4K keyframe
does not cost an allocation per frame.

**Back pressure, not silence.** A queue that grew without bound turns a badly interleaved
file — or a consumer that stopped asking — into memory exhaustion. So there is a cap per
stream, and reaching it makes the *other* consumer's `next` answer MP_ERR_BUSY: "somebody has
to drain before I can read more". Dropping the packets instead would be silent corruption;
blocking would be a deadlock between two consumers on different threads. `VideoGraph` treats
it as `Step::repeated` — nothing is available, so the picture that is up stays up — which is
the same thing it does when nothing is due.

The second front end earned its place again on the way: `Limits` was a nested struct with
default member initializers used as a default argument, which MSVC accepts and clang refuses
-- and `PassthroughConfig` carries a comment saying exactly that, from the last time. It is
at namespace scope now, and the check found it before the build did.

**The test is the equivalence.** What each stream gets through the router is compared, packet
for packet and byte for byte, against what it would have got from a demuxer of its own: same
count, same order, same bytes, while only one file position exists. Asked in turn, the way a
player asks, 68 packets are read for 68 delivered and none is left waiting. Drained one
stream at a time — the hard case — every audio packet in the file waits and then comes out
intact.

Two of the numbers that test first asserted were wrong, and both were the fixture rather than
the router. The audio track of `av.mp4` is **four thousand bytes in forty-four packets**, so a
four-kilobyte cap is the whole track and fills only after the file has run out; the cap test
uses 512 and stops part way through, which is what it meant to test. And a seek to half a
second lands on frame zero, because that fixture's only sync sample is its first frame —
§9.9's rule that a seek lands at or before the target, not a router that failed to move.

#### The loop that calls it, and the window it draws into

**Done, and a picture goes up.** `src/engine/mediaperch/display.hpp` is the loop --
`IFrameClock`, `IAudioClockSource` and `DisplayLoop` -- and `src/win/mediaperch/display_win`
is the two clocks and a window.

**Two clocks, and they are not the same one.** The audio device says where the sound is;
the display says when a picture may be drawn. Neither derives from the other -- a 60 Hz
display and a 24 fps film share no factor, and the sound card's crystal is not the monitor's
-- so the loop takes both and the policy between them is what is tested. `IFrameClock`
answers *when* and *what time it is* in one object, because they are one clock: the tick a
frame is drawn at is the tick the audio position must be extrapolated to, and taking them
from two sources puts a scheduling delay between them.

`VBlankClock` waits on `IDXGIOutput::WaitForVBlank`, on the output that actually contains the
window rather than the first the adapter enumerates -- which on two monitors at different
rates is the difference between pacing to the right display and pacing to a neighbour. It
makes its own DXGI factory, because the presenter has one and does not hand it out; the
alternative is an ABI addition to reach the swap chain, which is what
`GetFrameLatencyWaitableObject` would want and is the better source the day something needs
it. `TickClock` is the fallback and is not a bad one: a flip-model swap chain presented with
a sync interval of zero does not tear, so a loop that wakes often enough is smooth without
knowing when the display refreshes.

**The loop is not on the thread that owns the window**, and that is not a preference:
`WaitForVBlank` blocks for a whole refresh, and a message queue nobody drains for sixteen
milliseconds is a window Windows calls unresponsive. So the picture is painted on its own
thread and the main thread pumps messages.

**A seek is what the loop watches for.** It re-reads the graph's `clock_spec` every turn --
two atomic loads -- and reconfigures when the anchor moved, which costs one turn of no clock
rather than one turn of the wrong one. A device that stops answering does *not* cost the
clock: the last reading is still the best answer there is, and a loop that forgot it would
blank the picture on one missed read.

#### `mediaperch-probe show`

One file, one window, and §8 deciding when each frame goes up: one demuxer, the router
feeding both halves, the audio graph that owns the master clock, and the display loop reading
it. **One process, one window** -- §9.7.1's `MP_SURFACE_WINDOW` case, which is what a tool
has. The engine deliberately does not do this, for the reason §9.7.1 settles: a headless
engine that creates windows is not headless, and the daemon's picture will cross to a shell
as a DirectComposition surface instead. This is how the video path gets looked at before
there is a shell to look through.

Measured on this machine, all four decoders through one path:

| file | decoder | frames | dropped | turns | first late | worst after |
|---|---|---|---|---|---|---|
| `av.mp4` | codec_mft | 24 | 0 | 63 | 16.0 ms | 15.8 ms |
| `av1.mp4` | codec_dav1d | 24 | 0 | 60 | 8.5 ms | 16.6 ms |
| `vp9.webm` | codec_vpx | 24 | 0 | 60 | 8.3 ms | 16.6 ms |
| `av2.webm` | codec_avm | 16 | 0 | 40 | -- | -- |

Sixty turns for a one-second file on a 60 Hz display is the loop doing exactly what it says.
`av2.webm` states no frame rate -- Matroska wrote no default duration -- so the pacer measured
the interval from the timestamps, which is the path that exists for exactly that.

#### What "late" is, and where it comes from

It is **the frame's timestamp minus where the master clock said the stream was**, sampled at
the instant the loop woke and decided about that frame. Not when the pixels reached the
display: `present` hands the frame to the swap chain and the compositor shows it at the next
vertical blank after that, which is another refresh nobody here measures and DWM does not
report.

Reported as two numbers because it is two things, and only one of them is a pacing error:

- **The first frame** carries however long the clock had been running before anything was
  decoded. In these runs that is `--no-audio`'s doing: `WallClock` starts before the loop, so
  the setup and the first decode happen on its time. With a real audio device it does not
  arise -- the device's position starts at zero with the stream.
- **Everything after that** is the steady state, and the measurement is the whole answer:
  **16.6 ms against a 16.7 ms refresh**. It is display granularity and nothing else. A frame
  that becomes due a tenth of a millisecond after a vertical blank cannot be decided about
  until the next one, so "up to one refresh late" is the floor of a loop that decides at
  refresh boundaries. codec_mft's 15.8 ms is the same number on a run whose turns did not
  land quite the same way.

**Made, and measured again.** `VideoPacer::set_lead_seconds` is how long after a decision the
frame will be on screen, and `decide` now measures against that instant rather than against
the moment of waking -- and rounds to the *nearest* presentation rather than flooring to the
next one, because a frame due less than half a refresh after this presentation is nearer to
this one than to the one after it. `DisplayLoop` measures the refresh from the gaps between
its own turns and sets the lead; measured rather than asked, because a mode that calls itself
60 Hz is 59.94 and that is a frame every seventeen minutes -- the same rounding §9.9 refuses
for a container's frame rate. Taken as the shortest gap seen: a gap can only be lengthened by
a turn that was late, so the shortest is the one that was not.

| file | before | after | refresh measured |
|---|---|---|---|
| `av.mp4` | 15.8 ms late | 8.4 late .. 8.1 early | 16.33 ms |
| `av1.mp4` | 16.6 ms late | 4.3 late .. 5.6 early | 16.49 ms |
| `vp9.webm` | 16.6 ms late | 2.4 late .. 7.7 early | 16.47 ms |
| `av2.webm` | -- | 2.2 late .. 7.5 early | 16.41 ms |

The worst of them is 8.4 ms against a 16.3 ms refresh, which is half a refresh to the
millisecond: the claim and the measurement are the same number. A frame is now sometimes
shown *early*, which the old rule could not do and which is the whole of the improvement --
half a refresh early beats half a refresh late when the alternative was a whole one.

Zero lead stays the default, and it is what a caller with no display wants: show a frame once
it is due and not before, which is what every test that measures the arithmetic asks for.

#### How large a picture, measured

The presenter was asked to configure and to take one frame at each of ten sizes, on WARP and
on the hardware:

| | up to 16384 wide | 16385 wide |
|---|---|---|
| WARP | configure and present | configure ok, **present refused** |
| hardware | configure and present | **configure refused** |

**16384 is Direct3D 11's texture limit**, not this tree's and not this machine's, and it is
comfortably past 16K DCI at 15360x8640. So 5K, 6K, 8K, 10K and 12K are all simply pictures as
far as everything above the decoder is concerned; nothing here has a resolution in it.

What runs out first is elsewhere, and the numbers are worth having:

- **Memory per frame**, measured at eight-bit 4:2:0: 12 MB at 4K, 47 MB at 8K, **190 MB at
  16K**. Ten bits doubles it. A decoder's reference pool is eight to sixteen of those, so 16K
  ten-bit is three to six gigabytes before anything is presented.
- **Codec levels** cap lower than the presenter does. HEVC's highest level allows 35,651,584
  luma samples -- 8192x4352 -- so a conformant HEVC stream cannot reach 16K at all.
- **`PacketRouter`'s cap** is 32 MB per stream by default, and a single 16K keyframe could be
  larger than that. It still works: the cap is exceeded by at most the one packet that
  discovers it, because a packet already read cannot be put back.



**`--no-audio`, and what it admits.** §8's clock is the audio device, and a file with no audio
track has none; neither does a run on a machine whose endpoint refuses every format, which is
not hypothetical -- this one did while the above was measured. `WallClock` counts the
performance counter and reports it as though a device were playing, the program says which of
the two clocks it is using, and the header says plainly that it is not §8's. What it costs is
what §8 was avoiding: the counter and the display are not the same crystal either. What it
does not cost is anything a person can hear, because there is nothing to hear.

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

### 9.9.1 HLG is not PQ, and it does not pass through

Written down because the first version of `plan_for` got it wrong in a way that would have
looked like a tone mapping fault. It put PQ and HLG in one branch -- both are HDR transfers,
so both took the "the display can show it" path -- and on an HDR display with nothing
composited it chose a PQ buffer for both, under a comment that said *pass PQ through*.

**PQ states absolute nits. HLG does not.** ST.2084 maps a code value to a luminance, full
stop. ARIB STD-B67 is scene-referred: its OOTF is a system gamma derived from the *display's*
peak luminance, so the same signal is deliberately a different picture on a 600-nit panel and
a 1000-nit one. HLG code values written into a buffer tagged PQ are a picture that is far too
dark and wrongly graded, and nothing about the failure points at the transfer function.

**And no platform here presents HLG directly.** DXGI has no HLG swap chain colour space --
`YCBCR_STUDIO_GHLG_TOPLEFT_P2020` exists for video surfaces and not for presentation -- and
`CAMetalLayer` has none either. So HLG is linearised like everything else, which costs it the
packed buffer. That is the price of the format rather than a limitation of this module, and
it is worth stating in both places it will be looked for.

Two consequences in the code. `Plan` gained a **`Convert`** alongside `ToneMap`, because
conflating them is what caused this: tone mapping reduces dynamic range a display cannot
show, while a transfer conversion changes how numbers are coded and reduces nothing -- and
HLG needs one on a display that can show every stop of it. And `Display` gained
`peak_nits`, read from `DXGI_OUTPUT_DESC1::MaxLuminance`, because the OOTF is a function of
it; the plan carries the value so a shader never has to ask a display anything.

The one genuine pass-through in §9 is therefore narrower than it looked: **PQ content, on an
HDR display, with nothing composited over it.** Everything else is converted.

### 9.9.2 Two transfer curves, and which one a stream gets

Decided while writing the shader, because a renderer cannot decode a frame without answering
it, and it is the kind of thing that is invisible until two players are side by side.

**Content tagged sRGB gets the sRGB piecewise curve. Video tagged BT.709 or BT.601 gets
BT.1886, a pure 2.4 power.** BT.709 states a camera OETF; the display EOTF those standards
specify is BT.1886, and that is what the picture was graded on. Decoding video with the sRGB
curve instead lifts the shadows -- about twelve per cent at middle grey -- which is the
mirror image of the fault §9.2 records Windows committing in the other direction.

The code point decides, so nothing is guessed: `MpVideoInfo::transfer` comes from `colr` or
from the resolution convention, and the shader takes a flag rather than a policy. A test
renders the same grey both ways and holds each to its own curve.

**And the YUV matrix is derived rather than tabulated**, in `double`, once, at present time --
which is §9.10's rule about where double precision belongs, and the first place it buys
anything. Every coefficient is a ratio of the two luma weights a standard states, so
transcribing eight numbers per matrix by hand is how a digit goes missing. The test computes
the same conversion independently from Kr and Kb and holds the shader to it at 1e-5.

Studio range is the default and the flag is honoured: treating 16..235 as 0..255 crushes the
blacks and clips the whites, which reads as a contrast setting rather than as a bug.

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

### 9.7.1 Who owns the window, and how a frame crosses a process

**Decided: the engine renders, the shell hosts.** The alternatives were the engine opening
its own window, or the shell owning an `HWND` and handing it over. Both put something on the
wrong side of §10's line -- a headless engine that creates windows is not headless, and a
presenter that draws into a window belonging to a process that may be killed at any moment
has to survive that killing anyway. So the frame crosses the boundary instead of the window
crossing it.

The mechanism is a **DirectComposition surface handle**, which is what browsers use for
exactly this and is the one documented way to compose a surface produced in one process into
a window owned by another:

```
  mediaperchd                                    mediaperch-shell
  ------------------------------                 ---------------------------
  DCompositionCreateSurfaceHandle  ── HANDLE ──▶  IDCompositionDevice::
        │                          (over the      CreateSurfaceFromHandle
        │                           existing            │
        ▼                            IPC)               ▼
  IDXGIFactoryMedia::                             IDCompositionVisual::SetContent
  CreateSwapChainForCompositionSurfaceHandle            │
        │                                               ▼
        ▼                                         IDCompositionTarget on its HWND
  video_d3d11 renders and Presents                      │
                                                        ▼  Commit()
                                                   the picture
```

Four properties follow, and they are the reason for the choice rather than pleasant side
effects.

**The engine still has no window and no toolkit.** `CreateSwapChainForCompositionSurfaceHandle`
takes no `HWND`; `DCompositionCreateSurfaceHandle` is a free function and needs no
composition device, so the engine links one entry point from `dcomp.dll` and builds no visual
tree. The visual tree, the target and the `Commit` are all the shell's. §10 is untouched.

**The shell can die mid-frame.** The surface handle belongs to the engine. A shell that
crashes takes its own visual tree with it and the engine goes on rendering into a surface
nobody is showing; a shell that starts up calls `CreateSurfaceFromHandle` on the same handle
and the picture reappears. **No renegotiation, no device loss, no gap** -- which is the video
half of the test §10 already asks for on the audio side, and it is a test rather than a hope.

**One device, shared with the decoder.** This is the coupling that has to be decided rather
than discovered: a hardware video decoder produces textures on a particular `ID3D11Device`,
and handing them to a presenter on a *different* device costs a shared handle and a fence at
best and a copy at worst. **The presenter creates the device and the decoder is handed it**,
because a presenter is made once and outlives every decoder a playlist goes through, and
because the presenter is the one that has to satisfy `IDXGIFactoryMedia`. In ABI terms that is
an opaque platform pointer moving from `MpVideoVtbl` to whatever decodes -- the same kind of
thing `open` already takes.

**D3D12 is a second module, not a rewrite.** `IDXGIFactoryMedia` takes an
`ID3D12CommandQueue` for a D3D12 swap chain and the composition side does not change at all,
so `modules/video/d3d12` sits beside `modules/video/d3d11` behind one `MpVideoVtbl`, shares
`colour_plan.hpp` unchanged, and is chosen by priority or by name -- exactly as `sink_asio`
sits beside `sink_wasapi`. That is what the module boundary was for.

#### What the ABI needs, and when

`MpVideoVtbl::open` takes `void* window`, which was right for a presenter that either has an
`HWND` or has nothing. Under this architecture it has a third case, and it is the important
one: it *produces* a surface rather than consuming a target. So:

```c
typedef uint32_t MpSurfaceKind;
enum {
    MP_SURFACE_OFFSCREEN = 0u, /* no display; read_back is the output */
    MP_SURFACE_WINDOW    = 1u, /* `handle` is an HWND -- one process, one window */
    MP_SURFACE_COMPOSED  = 2u  /* the presenter makes a shareable surface */
};
MpResult (*open)(MpSurfaceKind kind, void *handle, MpVideo **out);
/* MP_ANY. The handle a host passes to whatever composes it: a DComp surface
 * handle on Windows, a dmabuf on Wayland, an IOSurface on macOS. */
MpResult (*get_surface)(MpVideo *v, void **out_handle);
```

**Written down now and implemented with the shell**, for the same reason ABI v3 was decided
before it was written: the shape is what would have been expensive to discover late, and a
vtable entry with no implementation is the mistake this tree has already made once and
reverted. Nothing composes anything yet.

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

### 9.8.1 One device, and why the decoder is per-API

**A decoder cannot be graphics-API-agnostic, and finding out where that bites decided the
module boundary.** Media Foundation binds a transform to a GPU through
`IMFDXGIDeviceManager`, which wraps an `ID3D11Device` -- **there is no D3D12 form of it.**
D3D12 video decoding is `ID3D12VideoDevice::CreateVideoDecoder` and a different command list
entirely. So a host that loaded a D3D12 presenter and `codec_mft` would have two devices and
a copy between them, which is the whole cost hardware decoding exists to avoid.

The ABI asks rather than assumes. `MpVideoCodecVtbl::probe` takes an `MpGraphicsApi`
alongside the codec, and `codec_mft` scores **0 for `MP_GRAPHICS_D3D12`** -- so a host picks
the decoder that matches the presenter's device instead of discovering the mismatch as a
frame rate. A D3D12 decoder is a sibling module, not a flag in this one.

`MpGraphicsDevice` is how the device travels: an api, a device pointer, and a queue for the
APIs that have one. **The presenter creates it and the decoder receives it**, through
`MpVideoVtbl::get_device`, because a presenter is made once and outlives every decoder a
playlist goes through.

**And the software path is not a consolation.** With no device, Microsoft's software H.264
transform produces NV12 in system memory -- which works on a machine with no usable adapter,
on a CI runner, and deterministically, so a decoded frame is something a test can hash rather
than look at. That is the path every test here runs.

Three things the first run of that test found:

- **The software transform does not implement `IMF2DBuffer2`.** It has the older
  `IMF2DBuffer`, and a decoder that only knew the newer one refused every frame. There are
  three ways to reach a buffer's rows and a decoder implements one of them; the pitch is what
  makes it worth asking, because NV12 out of a decoder is padded to a width the hardware
  liked and rows are contiguous only by luck.
- **`avcC` to Annex B is a container question wearing a codec's clothes.** MP4 stores H.264
  as length-prefixed NAL units with the parameter sets out of band and every decoder wants
  start codes with the parameter sets in the stream. The conversion is this module's, because
  `demux_mp4` hands over the container's bytes verbatim and that promise is what makes it
  checkable -- and it is a header of its own, tested over bytes, with no Media Foundation and
  no GPU in sight.
- **The parameter sets go back in front after a reset**, not only at the start. A flushed
  decoder has forgotten them and the first packet after a seek is a keyframe whose SPS and
  PPS live in the container.

#### DXVA2, D3D11 and D3D12 are three APIs over one specification

Worth naming precisely, because the names are three generations deep and get used
interchangeably -- including by an earlier version of this document.

**DXVA2 is a Direct3D 9 API.** `IDirectXVideoDecoder` and `IDirectXVideoDecoderService`,
introduced with Vista, and nothing in this tree touches it. The Direct3D 11 equivalent is
`ID3D11VideoDevice` and `ID3D11VideoDecoder`, which FFmpeg calls `d3d11va`; Direct3D 12's is
`ID3D12VideoDevice`. What all three share is the DXVA **specification** -- the decoder GUIDs,
the bitstream buffer layout, the picture parameter structures -- so the protocol is DXVA and
the API to reach it is whichever Direct3D you are on. An MFT sits above all of it.

**And a decoder's texture can be sampled, which was written down here as unknowable and is
not.** An MFT allocates its own output, by default as a texture with `D3D11_BIND_DECODER` and
nothing else -- which no presenter can view, so the frame would have to be copied and the
point of decoding on the GPU would be gone. `MF_SA_D3D11_BINDFLAGS` on the transform's output
stream attributes is the documented way to say what the allocation needs, and `codec_mft`
asks for `D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE`. A driver that declines gives back
a texture without the flag and the presenter says so by name rather than copying quietly.

The presenter adopts such a texture with two views over the one resource -- `R8_UNORM` for
the luma plane and `R8G8_UNORM` for the interleaved chroma, at the array slice the frame
names, because a decoder hands out one array and an index into it. Nothing is read back,
copied or converted. One test makes exactly that texture on the presenter's own device and
hands it over, which also exercises `get_device` and is the case a machine with no GPU can
still run.

#### WARP is the tests' choice, not the machine's limit

An earlier version of this section said the binding could not be checked here "because WARP
has no hardware decoder". Half of that is true and the conclusion drawn from it was not.

WARP really has no video device: no `ID3D11VideoDevice`, no decoder profiles, and
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` fails against it with `DXGI_ERROR_UNSUPPORTED` rather
than yielding a device without it. But **WARP is what the tests ask for**, by name and on
purpose, because a hash of its pixels means the same thing on every machine. The development
machine has an Intel Iris Xe with 80 decoder profiles, and asking it was always possible.

Asked, it says this:

| | |
| --- | --- |
| decoder found | `Microsoft H264 Video Decoder MFT`, `MF_SA_D3D11_AWARE = 1`, **synchronous** |
| frames returned | 24 of 24 as textures, none in system memory |
| the texture | `DXGI_FORMAT_NV12`, an array of **11 slices**, one frame per slice |
| bind flags | `0x208` -- `D3D11_BIND_DECODER \| D3D11_BIND_SHADER_RESOURCE`, both |
| the two views | `R8_UNORM` and `R8G8_UNORM` over it, `S_OK` each |

So `MF_SA_D3D11_BINDFLAGS` is honoured, and the zero-copy path is real rather than argued
for. A second test decodes the fixture on the hardware device and holds the picture to the
same evidence the system-memory chain is held to; it skips, saying which, on a machine whose
presenter device has no `ID3D11VideoDevice`.

Two things the asking changed:

- **The presenter's device is made for two jobs.** §9.8.1 puts it there because a presenter
  outlives every decoder a playlist goes through -- which means it must be created with
  `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, and made multithread protected, neither of which the
  presenting half needs and neither of which can be added afterwards. Media Foundation
  decodes on threads of its own, and a shared device that does not serialise its own use
  fails as intermittent corruption rather than as an error. The video flag is asked for on
  hardware only, and an adapter that refuses it still presents.
- **An asynchronous MFT is a different program.** `MFTEnumEx` marks one with
  `MF_TRANSFORM_ASYNC`; it must be unlocked and then driven by `METransformNeedInput` and
  `METransformHaveOutput` events, and until it is, every `IMFTransform` method returns
  `MF_E_TRANSFORM_ASYNC_LOCKED`. `codec_mft` drives no event queue, so it passes such a
  transform over rather than activating one and failing at `SetInputType` with the software
  fallback already spent. Several vendors ship async hardware decoders; that path is a
  follow-up, and it costs less than it sounds because Microsoft's own H.264 transform is
  synchronous *and* D3D11-aware -- on this machine it **is** the DXVA host, not a software
  fallback, which is why one decoder covers both columns of the table above.

#### What a D3D12 decoder actually costs

Since one is planned, the difference is worth knowing before it is started, and it is larger
than the API names suggest.

`codec_mft` gets a whole decoder: an `IMFTransform` takes an Annex B bitstream and hands back
frames. **D3D12 Video does not work that way.** `ID3D12VideoDecoder` takes the DXVA picture
parameters and slice control data, which means the *host* has parsed the sequence, picture
and slice headers -- so a `codec_d3d12video` module contains an H.264 (and then an HEVC, and
then an AV1) bitstream parser before it decodes anything. That is what FFmpeg's `d3d12va`
hwaccel is, and it is most of the work.

Two things do get easier. The output resources are **created by the caller**, so the binding
question above simply does not arise -- you allocate them with what you need. And a parser
written for it is the same parser a software decoder would want, so it is not thrown away.

There is a middle road worth naming so that it is a choice rather than a discovery: decode on
D3D11 through `codec_mft` and share the texture with a D3D12 presenter through a shared NT
handle and a fence. It works, it is what several browsers do, and it is exactly the two
devices and the synchronisation §9.8.1 says to avoid -- which is a smaller cost than a copy
and a larger one than nothing. Which of the two a D3D12 presenter takes is a decision for
when there is one.

### 9.8.2 Which decoders, and can a software one use the GPU

**openh264, dav1d and their neighbours are separate modules, not replacements.**
`MP_KIND_VCODEC` was made a kind rather than a flag so that this is a matter of loading a
different module, and the audio side is already this shape: `codec_flac` is libFLAC,
`codec_mpa` is libmpg123, `codec_aac` is this tree's own Rust. Video will be the same.

**And once they are here, `codec_mft` is the hardware decoder and nothing else.** That is a
decision rather than an observation, and it has an order to it: `codec_mft` currently claims
`MP_GRAPHICS_NONE` at 80 and falls back to Microsoft's software transform when no hardware
one takes the device, which is what every video test in this tree rides today. Demoting it
before there is another software decoder would leave a machine with no GPU unable to decode
at all -- so the software module lands first, the score comes down second, and the fallback
inside `activate` goes with it. Then `open` returning MP_ERR_UNSUPPORTED on a machine with
no video device is the registry working as designed rather than a failure.

What that buys is the thing §7 says about audio, arriving for video: the deterministic
decoder is one in the tree, checkable the way `codec_alac` is, rather than a black box that
a Windows update can change under a hash.

#### The container half of AV1 is done; the library is a build-system question

**Done:** `MP_CODEC_AV1` is appended, `demux_mp4` recognises an `av01` sample entry, and the
`av1C` record crosses verbatim. Bento4 parses `av1C` into fields and — unlike `avcC` and
`hvcC` — keeps no raw bytes for it, so the record is read the way `alac_config` reads the
ALAC cookie: the box is asked to write itself and what follows the eight byte header is the
record. Rebuilding it from the parsed fields would work and is the wrong shape, because the
ABI says a codec gets the container's blob **verbatim** and a reassembled record is this
module's opinion of the file rather than the file.

`tests/data/mp4/av1.mp4` is the same 128x96, the same twenty-four frames and the same audio
as `av.mp4`, in AV1 rather than H.264, so the two files differ in the codec and in nothing
else. SVT-AV1 encodes it: libaom is the reference and is minutes rather than seconds here,
and what is under test is the container.

#### codec_aom: the reference, and what nesting a third-party build costs

**Done.** libaom decodes the same fixture dav1d does, and `av1_cross_test.cpp` holds the two
against each other **byte for byte**. AV1's decoding process is defined bit-exactly, so a
conformant decoder has one right answer for every sample -- which makes two independent
implementations agreeing on 442 kilobytes a far stronger statement than anything a single
decoder's own tests can make. `codec_dav1d_test.cpp` rules out a cleared buffer and a dropped
chroma plane; this rules out being subtly, consistently wrong. It is §12's method for audio
arriving for video.

libaom scores 40 against dav1d's 100 and is not meant to play anything. A reference decoder
is slow because clarity is what makes it a reference, and §7's rule that an explicit choice
wins outright is how a person asks for the reference answer.

**And `add_subdirectory` was the wrong answer for a library that builds with CMake**, which
took three tries to establish. libaom calls `enable_language(ASM_NASM)` from inside its own
tree, and CMake requires that call in the highest directory common to every target using the
language; nested, it generates a build with no rule to assemble anything --
`CMAKE_ASM_NASM_COMPILE_OBJECT` missing, nine times. Hoisting the call to this tree's top
level fixed that and started a cascade, because two other submodules do the same thing in
their own corners: external/wavpack enables ASM_MASM and external/mpg123's `project` line
asks for ASM. Enabling those at the top level too got as far as `MSVC_RUNTIME_LIBRARY value
'MultiThreadedDebugDLL' not known for this ASM compiler` **from inside wavpack**, which had
been configuring perfectly well until this tree started enabling languages on its behalf.

The same libaom configured standalone works under both Ninja generators first time, which is
what located it: the fault is the nesting and nothing else. So libaom is an external project
with its own CMake invocation, its own assembler and its own runtime setting, and this
tree's language configuration is exactly what it was. `enable_language` is a global act, and
a submodule's build is not this tree's to hoist.

Two more things worth recording:

- **CMake takes the first assembler it finds**, which on the development machine is the YASM
  inside a Perl distribution -- identified as YASM, with no compile rule, and the same nine
  errors with no hint about which program it meant. NASM is named rather than searched for.
- **libaom cannot answer `get_format` before the first frame**, unlike dav1d: there is no
  public way to parse a sequence header without decoding. It returns MP_ERR_BUSY, and a host
  that needs the geometry earlier asks the demuxer, which read it out of the container.

#### H.264 is declined before it is opened, and MFShutdown deadlocked

**Done.** `avcc.cpp` -- since moved to `modules/shared/h264`, because none of it was ever
Media Foundation's -- reads a sequence parameter set -- Exp-Golomb over an RBSP, with the
emulation prevention bytes taken back out -- and stops after `profile_idc`,
`chroma_format_idc` and the two bit depths. `codec_mft`'s `probe` scores **0** for anything
that is not 4:2:0 at eight bits, and `open` refuses it a second time with a sentence.

That is where the refusal has to happen. §7's rule is that a decoder failing mid-file must
not trigger a silent retry with another backend, which only works if the declining is early
enough for a host to look elsewhere -- and the `avcC` carries the answer before a decoder is
opened. What it declines is measured rather than assumed: enumerating this machine's D3D11
decoder profiles gives `H264_VLD_NOFGT` and no 4:2:2 or 4:4:4 entry, and Media Foundation's
software transform is 4:2:0 too.

A config that cannot be read leaves the score alone. "I could not tell" is not evidence of a
format that cannot be decoded, and refusing on it would decline files that work.

The tests write the SPS bit by bit rather than pasting a hex blob, so the syntax is stated
where a reader can check it against the standard and the Exp-Golomb coding is exercised in
both directions.

**And it found a deadlock that had been waiting for a caller.** `codec_mft` started Media
Foundation from a lazy static and stopped it from that static's destructor -- which runs
during `FreeLibrary`, under the loader lock, while `MFShutdown` waits for Media Foundation's
worker threads, which need that lock to exit. Reproduced in twenty lines outside any test
framework: load the module, call `open` so that MF starts and then fails before activating a
transform, call `FreeLibrary`. It hangs. Create and release a transform first and it does
not, which is why the fault survived until a `probe` that declines a stream gave `open` its
first way to fail after MFStartup.

Two halves to the fix, and both were real:

- `MFShutdown` moved to `module_shutdown`, which is what the ABI has it for -- the host calls
  it before unloading, off the loader lock.
- **The test harness was not modelling the host.** `ModuleRegistry` calls `init` after
  loading and `shutdown` before unloading; every `Module` helper in `tests/` did neither, so
  no test had ever exercised the shutdown path. They call it now, which is what let the
  MFShutdown move to where it belongs rather than being worked around.

#### codec_dav1d, and the one submodule CMake cannot build

**Done.** `codec_dav1d` decodes AV1 through dav1d, and the chain runs end to end: `demux_mp4`
reads the container, dav1d decodes to planes, `video_d3d11` converts by the matrix the
container named, and `read_back` hands over single-precision linear light.

**Meson, as an external project, and it is a real build dependency.** dav1d builds with
Meson and nothing else — 38 C sources of which 13 are compiled twice for the two bit depths,
47 nasm files, and no CMake anywhere in its tree — against thirteen submodules here that are
all `add_subdirectory`. Writing a CMakeLists for it would mean reimplementing the bitdepth
templating and the assembly rules and then maintaining them; dropping the assembly to make
that tractable would leave a decoder with dav1d's name and not its speed, which is the only
reason to prefer it over libaom. So `meson`, `ninja` and `nasm` are required, the module is
skipped loudly when they are absent, and the tree still builds without an AV1 decoder — the
shape every optional module here already takes. On the development machine they come from a
micromamba environment, which is also how the Python problem was solved: the only Python on
PATH was Inkscape's, with no `pip`.

Three things this turned up that were not obvious from the outside:

- **The release CRT, always, and §4 is what makes it safe.** dav1d is built once as a
  release static library rather than once per configuration. That leaves a Debug MediaPerch
  linking a release-CRT archive, which would be a mixed-runtime bug in ordinary code and is
  not one here: the module is a DLL of its own and §4's rule that nothing is allocated
  across the module boundary means no CRT object ever crosses it. The rule was written for
  language interop and it paid for something else.
- **ExternalProject does not look at the submodule.** It stamps its configure and build
  steps and then trusts them, so a `git checkout` in `external/dav1d` leaves the built
  library at whatever it was, with nothing saying so. That is not hypothetical: the first
  build here was made while the submodule sat on master, stayed that way after it was pinned
  to 1.5.4, and `vcs_version.h` reading `1.5.4-2-gaa09a630` was the only thing that admitted
  the tests had run against two commits nobody had chosen. `BUILD_ALWAYS` is the fix and
  costs almost nothing, because Meson's own Ninja notices that nothing changed and returns.
- **Meson resolves wraps whether or not they are wanted, and writes into the source tree.**
  dav1d carries `subprojects/checkasm.wrap`; `enable_tests=false` does not stop the wrap from
  being resolved, so the first configure downloaded checkasm and a `.wraplock` *into the
  submodule*, which then showed as untracked content in it and would have been a network
  fetch during every CI configure. `--wrap-mode=nodownload` refuses the download and the
  lock file is deleted afterwards -- removed rather than hidden behind
  `submodule.<name>.ignore = untracked`, because a submodule that reports itself clean when
  it is not is worth less than one that does not.
- **`meson setup` needs a different argument depending on whether it has run before** —
  `--reconfigure` for a configured directory, `--wipe` for a configured one it should
  discard, and neither works on a fresh one. Emptying the directory first would settle it and
  cannot be done from a script `ExternalProject` runs *inside* that directory, which is what
  the first attempt did and why it failed. Asking is what is left, and
  `meson-private/coredata.dat` is the file that answers.
- **MEDIAPERCH_ARCH does not reach dav1d, and that is the right answer.** dav1d builds
  every SIMD variant and picks one with CPUID, so the compiler's baseline never touches its
  assembly. `dav1d_set_cpu_flags_mask` can cap it, and capping it was written and then
  removed, because measuring what the call actually does answered the question that prompted
  it — *does the AVX2 build then stop going through SSE code?*

  **It does not.** dav1d's dispatch is a cascade of overwrites rather than a choice of one
  tier: a DSP init assigns the SSSE3 functions, then overwrites the ones that have AVX2
  versions, then the ones that have AVX-512 versions. `loop_filter_dsp_init_x86` is the
  example to read — with AVX-512 and a slow gather, two of its four entries stay at AVX2 —
  and every function with no AVX2 version keeps its SSE one and runs it. So an AVX2 build
  goes through SSE code paths whatever the mask says, and nothing short of dav1d
  implementing everything twice would change it.

  What the mask did do, exactly: on the `avx2` build it set all six flags, which is dav1d's
  default, so it was a no-op; on the `baseline` build it removed AVX2 and AVX-512, making
  that build slower on a modern CPU for no correctness gain, since dav1d's assembly is
  bit-exact against its C. The opposite of skipping wasted work, which is what it was for.

  So dav1d is left to dispatch — which is what §CompilerOptions already records for
  libFLAC, libmpg123, libopus and libwavpack. This tree compiles *its own* inner loops twice
  because it controls that codegen; a library's own is not ours to pick.

**CI installs them, and finding out that it did not is the reason.** The runner image has
Ninja and neither of the other two, so the `find_program` guard fired and `codec_dav1d` was
skipped in every leg -- the build stayed green and the artifacts quietly had no AV1 decoder,
which is the kind of silent absence this repository does not accept anywhere else. Both
tools now come from the Miniconda the image ships, rather than from pip, so they arrive from
one channel and the same one the development machine uses. The step sits *before* the
developer environment for the reason the FFmpeg step in `quality` does: that step captures
PATH into `GITHUB_ENV`, and a PATH captured before an install is a PATH without it. A
version report follows it, because a skipped module looks exactly like a passing build.

**And it is the producer ABI v4 was written for.** dav1d hands back 4:0:0, 4:2:0, 4:2:2 and
4:4:4 at 8, 10 and 12 bits, with the significant bits at the **bottom** of the container
where P010 puts them at the top. Until now every planar frame the presenter had seen was
built by hand in a test, which proves the arithmetic and not the join; `codec_dav1d_test.cpp`
is where a real decoder's frames meet it.

Four reasons one would be wanted beside `codec_mft`:

- **Codecs Windows does not have.** AV1 needs Windows 11 and a Store extension; VVC has
  nothing at all. **dav1d** is the answer for the first and is BSD-2.
- **Determinism.** MF's software decoder is a black box that can change with a Windows
  update. A decoder in the tree is checkable the way `codec_alac` and `codec_aac` are, which
  for a project whose method is hashing output is not a small thing.
- **The other platforms.** The Linux head needs a decoder that is not Media Foundation, and
  the same one runs everywhere.
- **openh264's licence is more specific than it looks.** Cisco pays the H.264 pool for *the
  binary they publish*; building the source yourself does not carry that, which is a
  distinction worth reading carefully before treating BSD-2 as the whole answer.

**Can any of them use the GPU? For the decoding itself, no.** openh264, dav1d and libaom are
CPU with hand-written SIMD, deliberately: entropy decoding -- CABAC, CAVLC, AV1's symbol
decoder -- is serial by construction and maps badly onto a GPU. What uses the GPU is the
fixed-function video block, NVDEC and VCN and Quick Sync, reached through DXVA2, D3D12 Video,
Vulkan Video or VA-API -- which is `codec_mft` and its siblings, and is different silicon
rather than the same decoder running somewhere else.

**What can go to the GPU is everything after the decode**, and it already does. The colour
conversion is a shader here. AV1 film grain synthesis is the other obvious one: dav1d does it
on the CPU, it is embarrassingly parallel, and a presenter is where it would belong -- which
is an argument for the split this ABI already makes rather than against it.

#### codec_vpx: VP8 and VP9, and the one build that wants a second Unix

**Done.** Both codecs, out of a WebM, through `demux_mkv`, `codec_vpx` and `video_d3d11`.
libvpx is the reference implementation for both, which is §7's argument for libFLAC and the
Xiph libraries applied to video: where a reference implementation *is* what the codec means,
it is worth a dependency. VP8 is decoded on its own account rather than as a by-product --
reading only the newer of the two would leave half the reason libvpx is here at all.

VP9 is not a receding format on the evidence either. This machine's GPU carries VP9_PROFILE0
and VP9_10BIT_PROFILE2 in its fixed-function block and Microsoft ships a VP9 extension, which
makes `codec_mft` a possible second opinion the day someone wants one -- the way libaom is
dav1d's.

It is also the first exercise of something `demux_mkv` did not have. Every video track in a
Matroska came back `MP_CODEC_UNKNOWN` until this work: the container this tree splits best
was the one it could decode least, which under §12's container-first resolution means a
demuxer that parses perfectly and hands over a payload nothing can claim.

**The build needed MSYS2, and the reason took four wrong answers to reach.** libvpx has
neither CMake nor Meson: `configure` is a POSIX shell script, `make` generates Visual Studio
projects from what it wrote, and MSBuild builds those. That much is only awkward. What is not
optional is *which* Unix.

`libs.mk` hands the entire source list to `gen_msvs_vcxproj.sh` on one command line. A native
Windows GNU make runs that through cmd.exe, whose limit is 8191 characters; the list is about
11,800. **Nothing reports it.** The truncated command exits zero, writes a plausible project,
and produces an archive that fails at link time on symbols from its own objects.

Ground truth came from making the generator print its own arguments: 223 of them, 7917 bytes,
the last cut mid-word at `../libvpx/vpx_dsp/x86/inv_txfm`, and 7917 plus the leading options
is 8191 exactly. Which files fall off is not arbitrary either -- `libs.mk` names vp8, vp9,
vpx and vpx_dsp in explicit filter clauses and sweeps the rest up in a filter-out clause
last, so what is lost is always vpx_mem, vpx_ports, vpx_scale and vpx_util. `vpx_calloc` was
the symbol that reported it, several steps downstream of the fault.

Four things were believed before that measurement and none survived it: that the submodule
was at the wrong version, that the build directory's path was too deep, that the command
being measured was the one that failed, and that `SHELL=` or `MAKESHELL=` pointing a native
make at a POSIX shell would be enough. The last is the closest to right and still wrong: the
whole userland has to be MSYS2's, because libvpx's scripts use its `sed`, `cut` and `cygpath`
too. Under MSYS2's make the generator receives 319 arguments and the archive goes from 110
objects to 157.

So this is the one submodule whose build wants a second Unix userland rather than one more
tool, and it is skipped loudly when that is absent -- the same guard `codec_dav1d` has, for
the same reason. CI's image carries MSYS2 at `C:\msys64` and deliberately keeps it off PATH,
which is the arrangement this wants anyway: `modules/codec/vpx` finds that bash and that make
by name, with `NO_DEFAULT_PATH` so that finding Git for Windows' bash instead is impossible
rather than unlikely. `make` is the one thing the image does not carry, and one `pacman` line
is the whole difference.

**Configured for what this tree can describe rather than for what libvpx defaults to.**
`--enable-vp9-highbitdepth` is off by default and is what makes VP9 profiles 2 and 3 -- ten
and twelve bits, 4:2:2 and 4:4:4 -- readable at all; leaving it off would let ABI v4 describe
depths this decoder then refused to produce. The encoders, `webm-io` and `libyuv` all go:
nothing here encodes, containers are demuxers here, and colour conversion is a shader.
`--enable-coefficient-range-checking` makes the decoder check its own intermediate transform
coefficients, which is what a build being fuzzed or audited wants and not what a shipping one
does, so it follows `MEDIAPERCH_DECODER_CHECKS` rather than being on or off for everybody.

`--enable-postproc` is the one that is off for a reason larger than this module, and §9.8.3
is where that goes.

### 9.8.3 The stage the video side does not have

**Audio is three stages and video is two.**

    audio:  demux -> codec -> DSP -> sink
    video:  demux -> vcodec ->  ?  -> video

`MP_KIND_DSP` sits between a codec and a sink, and §5 is built on what that
separation buys: Path A has no DSP in it at all and is bit-exact by
construction, Path B has whatever a person put there and **says so**. Neither
the decoder nor the sink can quietly process anything, because processing is
somebody else's module.

The video side has no equivalent, and the absence has already been felt twice.

- **libvpx's `--enable-postproc`** is deblocking and denoising *inside the
  decoder*. Switching it on changes what the decoder returns, which means a
  conformant decoder stops producing the specified picture and
  `av1_cross_test.cpp`'s whole method -- two implementations agreeing byte for
  byte -- stops working. It is off, and the reason it is off is that there is
  nowhere else to put it.
- **AV1 film grain synthesis** is the case §9.8.2 already argues about: it is
  embarrassingly parallel and belongs on the GPU rather than in dav1d's CPU
  loop. `MP_VIDEO_FILM_GRAIN` exists so that something downstream can know
  there is anything to move. There is nothing downstream.

And the reason to want one is not tidiness: the video engine is meant to carry
colour grading, which is a chain of stages a person assembles -- exactly what
`modules/dsp` is for audio.

**What it would have to be, and where it differs from the audio one.**

`MpDspVtbl` processes a deinterleaved `double` bus: planar, one pointer per
channel, frames in and frames out with a stated latency. A video stage cannot
copy that shape, for one reason that decides the whole design:

> **It has to be able to run on the presenter's device.**

A CPU stage taking planes and returning planes would force GPU decode -> system
memory -> GPU, which is the round trip §9.8.1's whole texture-adoption argument
exists to avoid. So a video DSP takes an `MpGraphicsDevice` the way
`MpVideoCodecVtbl::open` does, and `probe` takes an `MpGraphicsApi` so a host
can tell which stages will run on the device it has and which would drag a
frame back through memory.

The rest follows the audio shape: `configure` states what comes in and what
goes out, because a stage may change the layout -- a grader that works in
4:4:4 is entitled to say so and let the host decide whether to insert it.
`process` takes one frame and produces one, with the output valid until the
next call, which is the promise a decoder already makes. `set` and `describe`
are the same pair every other kind has.

**Not added yet, and that is the rule rather than laziness.** §15 says not to
add an interface until the second implementation of it exists, and §4 records
that a kind number with no vtable and no module is the mistake this tree made
once with `MP_ENCODING_DSD` and reverted. Nothing here processes video between
the decoder and the presenter today. The first stage that wants to -- film
grain moved off dav1d, or a lookup table, or a scaler -- is what should bring
the interface with it.

### 9.8.4 AV2, and a decoder that is the only one

**AV2 reached 1.0.0 on 2026-05-29.** The CHANGELOG has one line in it. dav2d
exists -- a submodule here already -- and is where a player's AV2 decoder will
come from, the way dav1d is for AV1; it has no module yet. So `codec_avm` is not
a second opinion the way `codec_aom` is. It is the whole of AV2 support, and a
breaking change in a 1.0.0-shaped research codebase is a thing to expect rather
than to be surprised by. The submodule is pinned to the tag, and `BUILD_ALWAYS`
is on for the reason external/dav1d records: an external project does not notice
its own source moving.

**It scores 40 anyway.** A reference implementation is correct and slow by
construction. A host with one AV2 decoder picks it at 40 exactly as it would at
100 -- a score only decides between rivals -- so scoring it 100 today would mean
editing two modules later to say what is already true now.

#### A media player very nearly acquired TensorFlow Lite

avm carries two machine-learning tools, both **on by default**:
`CONFIG_ML_PART_SPLIT` for partition search and `CONFIG_DIP_EXT_PRUNING` for
intra mode pruning. Each forces `CONFIG_TENSORFLOW_LITE` on with it, and that
block extracts and patches sixteen vendored tarballs -- TensorFlow Lite,
abseil, XNNPACK, protobuf, Eigen, flatbuffers, ruy and the rest -- through
`execute_process(COMMAND bash -c "... tar -xzf ...; patch -p1 < ...")`. On
Windows that wants a Unix userland as well, which `modules/codec/vpx` has only
just established as a dependency for one other module.

**All of it is encoder-side, and that was read rather than assumed.** The whole
TFLite block sits inside `if(CONFIG_AV2_ENCODER)` in avm's top-level
CMakeLists, and what `CONFIG_DIP_EXT_PRUNING` adds to the *decoder* is one extra
output from the intra predictor -- eleven downsampled edge pixels, written into
`mbmi->intra_dip_features`, which the encoder's model reads and the decoder does
not. It gates no syntax element. So the tools stay exactly at the reference's
defaults, the encoder is off, and the decoder builds in **1.3 minutes** with no
bash, no tar and no network.

**And the fixture is what proves that reading instead of trusting it.** The
encoder that produced `tests/data/mkv/av2.webm` was built with both ML tools
*off* while the module's decoder has them *on*. Had they been bitstream tools
rather than encoder search heuristics, the decoder would have failed on that
stream. It decodes, so they are not.

One hazard for anyone who does build avm's encoder on MSVC:
`avm_dsp/x86/highbd_variance_avx2.c` took **48 CPU-minutes in one cl.exe and had
not finished**. `-DENABLE_AVX2=0` skips it, and for a tool that encodes sixteen
frames of 128x96 once, losing AVX2 costs nothing -- the encode is 16 seconds.
That switch belongs to the fixture tool and not to the module, whose decoder
build never touches the file.

#### AV2 has no container binding yet, so this reads the one avm writes

There is no ISOBMFF binding at 1.0.0. What there is, is avm's own muxer:
`common/webmenc.cc` calls `set_codec_id("V_AV2")` outright and writes a
four-byte `Av2Config` into Matroska's CodecPrivate. Neither is in Matroska's
codec registry.

Reading them is not the same as inventing them. `demux_mkv` gained a `V_AV2`
line for the same reason it has the other five video ids: the alternative is
returning MP_CODEC_UNKNOWN for a string the reference implementation defines,
which would mean refusing to read the only AV2 files that exist. If the registry
lands on a different spelling, that is a line gained rather than a line changed.
The record is documented in `module.h` beside the others, including the part
that makes it unlike `av1C`: `get_av2config_from_obu` states that it does not
store the configuration OBUs, so an AV2 sequence header is only ever in the
stream and a decoder handed the record has nothing to feed itself from it.

#### Two things the AV2 fixture found

**avm has no `allow_lowbitdepth`, so eight bits arrive in sixteen.** libaom's
`aom_codec_dec_cfg_t` has that field and avm's does not: AV2's decoder always
takes the high bit depth path. dav1d, libaom and libvpx all hand an eight-bit
stream back in eight-bit planes; avm hands back sixteen with the samples at the
bottom. **ABI v3 could not have described that** -- `bool ten_bit` had one
question and two answers, and eight-in-sixteen is neither of them. v4 states it
as three numbers, `mp_pixel_sample_scale` divides by the ratio, and the picture
arrives right rather than sixty-four times too dark. M5.98 was argued for on
paper; this is the first producer that would have broken without it.

**`demux_mkv` refused every silent file.** `open` chose a default track by
looking for audio and returned MP_ERR_UNSUPPORTED when it found none, so a
screen recording, an animation, and every AV2 stream anybody can encode today
all read as a container this module could not parse. `demux_mp4` has had the
fallback line since it was written -- `d->selected = {0}; // a file with no
audio at all still opens` -- which makes this exactly the divergence §12 exists
to stop, sitting unnoticed because every video fixture in the tree carried Opus
beside the picture. The AV2 one cannot: nothing muxes audio next to AV2 yet.

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
| M6.7 | §8: the audio clock, and video against it | **done, and it reads no clock.** `AvClock` and `VideoPacer` take a device reading and a performance-counter tick and do arithmetic; neither calls QueryPerformanceCounter, touches a sink or knows what Windows is, which is what makes a device running fast, slow or stopped something a test can arrange. The rule that audio never moves is enforced by absence: there is no method that could. Three offsets sat between the file and the listener, each recorded in this tree and none of them read -- the device buffer that `position_frames` counts past (30 ms here, a frame and a half at 24 fps), the DSP chain's latency that `MpDspVtbl::get_latency` was appended for, and the anchor a seek moves, which the graphs have kept since gapless and nothing had compared against the device. Two thresholds and no third: shown when the clock reaches the timestamp, dropped when it has passed it by more than one frame interval, and repeat otherwise -- which is not an instruction, because a display given no new frame shows the old one. The interval is the container's ratio when stated and measured from timestamps when not, and nothing is dropped until two have been seen. Checked against av1.mp4 decoded rather than demuxed, because presentation order is what a decoder produces and not what the packets are in: at the right speed 24 shown and none dropped, half a second ahead twelve dropped and twelve shown, stopped a thousand polls and the picture holds |
| M6.6 | A pass over the whole tree before A/V sync | **done.** Every source parsed by a second front end with the tree's own warning set, the MSVC analyzer's findings read, clippy over the Rust, and the recent work re-read. The one that mattered: `threads = 0` is one thread in libvpx, libaom and avm -- each copies it into `max_threads`, and every multithreaded path is gated on `max_threads > 1` -- so VP8, VP9, AV1-by-reference and AV2 were all decoding on a single core under a comment saying the library would choose; dav1d's zero genuinely means auto, which is where the belief came from. All three take the machine's thread count now, from one shared header. Also found: codec_mft declared no codecs at all, which made it invisible to a registry reading declarations, and the module sweep now requires every codec to declare at least one; `verify`'s SHA-256 ignored a failed feed and would have vouched for a digest of most of the bytes; codec_mft's end-of-drain doubt stayed armed across a new run of packets; and matrix code point 0 -- identity, which is RGB planes -- fell through to BT.709 in the presenter and is refused with a sentence instead. Dead code out: a write-only field in the processed graph, an unread host pointer in nine modules, a byte-reader accessor nobody called, two test helpers, and the seven drifted copies of the test-side module loader, which are one header now. The clang check gained the DSP modules, which are portable and had never been under it. And the warnings became errors: /WX and -Werror on `mediaperch_flags`, which the root list file now links into every target outside external/ by walking the tree rather than by each target remembering to -- the ABI probes had not. Somebody else's code compiled inside the tree is a target of its own carrying MEDIAPERCH_EXTERNAL, which the walk skips and nothing else touches: dr_wav's implementation (once, not twice), four files of the VST3 SDK (once, not three times), and the registry calls whose SDK annotations the analyzer objects to, carved out of sink_asio. Headers are the one thing a target boundary cannot separate, so external submodules are added SYSTEM; that silences the compiler and, measured, not the analyzer, which is told separately with /analyze:external- -- the one flag that exists for somebody else's code |
| M6.5 | codec_avm: AV2, by the only decoder it has | **done.** AV2 reached 1.0.0 on 2026-05-29 and avm is the whole of its support -- dav2d is a submodule with no module, so unlike libaom this is not a second opinion. It scores 40 all the same, because a score decides between rivals and dav2d should outrank it later without an edit. Two of avm's defaults would have made a media player build TensorFlow Lite: CONFIG_ML_PART_SPLIT and CONFIG_DIP_EXT_PRUNING force it on, and its block extracts and patches sixteen vendored tarballs through bash and tar. All encoder-side -- the block sits inside CONFIG_AV2_ENCODER, and what DIP pruning adds to the decoder is eleven edge pixels the encoder's model reads -- so the tools keep the reference's defaults, the encoder goes, and the decoder builds in 1.3 minutes. The fixture proves that reading rather than trusting it: its encoder was built with both tools off against a decoder that has them on. AV2 has no container binding yet, so demux_mkv reads V_AV2 and the four-byte Av2Config avm's own muxer writes. Two things fell out: avm has no allow_lowbitdepth, so eight bits arrive in a sixteen-bit container -- a shape ABI v3 could not have stated at all and the first producer that needed v4 rather than merely suiting it -- and demux_mkv turned out to refuse every file with no audio track in it, which demux_mp4 has never done |
| M6.4 | codec_vpx: VP8 and VP9, by the reference implementation | **done.** Both, out of a WebM, through demux_mkv and video_d3d11 -- and the first time demux_mkv named a video codec at all, having returned MP_CODEC_UNKNOWN for every video track until now. The build is the finding: libvpx has neither CMake nor Meson, and its Windows build needs MSYS2 rather than merely preferring it. `libs.mk` hands the whole source list to `gen_msvs_vcxproj.sh` on one command line; a native make runs that through cmd.exe and its 8191-character limit truncates it silently, writing a plausible project whose archive then fails to link on `vpx_calloc` -- one of its own symbols. Measured by making the generator print its arguments: 223 of them, 7917 bytes, the last cut mid-word, and 7917 plus the options is 8191 exactly. Four explanations were believed and discarded before that, `SHELL=` included: the whole userland has to be MSYS2's, because libvpx's scripts use its sed and cut too. Under it the generator gets 319 arguments and the archive 157 objects rather than 110 |
| M6.3 | codec_aom: a second AV1 decoder, to hold the first to its word | **done.** libaom decodes the same fixtures dav1d does and `av1_cross_test.cpp` requires the two to agree byte for byte, on a clean stream and on a grainy one -- which is a stronger statement than either decoder's own tests can make, and the reason to carry a reference implementation that scores 40 at probe and is not meant to play anything. Nesting its build is what it cost: libaom calls `enable_language(ASM_NASM)` from inside its own tree, which CMake will not honour there, and hoisting the call cascaded into wavpack's ASM_MASM and mpg123's -- so it is an external project, configured standalone, which is where it works first time. CMake also picked Strawberry Perl's YASM as the assembler until nasm was named outright |
| M6.2 | H.264 declined before it is opened | **done.** An SPS reader in `avcc.cpp`, since moved to modules/shared/h264 -- Exp-Golomb over an RBSP with the emulation prevention bytes removed -- and a `probe` that scores 0 for any H.264 that is not 4:2:0 at eight bits, which is measured rather than assumed: this machine's D3D11 decoder profiles have no 4:2:2 or 4:4:4 entry and MF's software transform is 4:2:0 too. §7 needs the refusal at probe, because a decoder failing mid-file must not trigger a silent retry. It also found a deadlock waiting for a caller: codec_mft stopped Media Foundation from a static destructor, which runs during FreeLibrary under the loader lock while MFShutdown waits for threads that need it -- reproduced in twenty lines, fixed by moving it to `module_shutdown`, and only reachable at all because every test harness here had been loading modules without ever calling their shutdown |
| M6.1 | codec_dav1d | **done.** AV1 decoded by dav1d, end to end through demux_mp4 and video_d3d11, and the first decoder here to produce a planar frame -- so the presenter's planar path now has a producer rather than frames a test built by hand. Meson as an external project, which is a real build dependency and the first time CMake could not build a submodule; `meson`, `ninja` and `nasm` required, the module skipped loudly without them. dav1d is built once with the release CRT, which §4's no-allocation-across-the-boundary rule is what makes safe. MEDIAPERCH_ARCH is deliberately not plumbed through: capping dav1d with `dav1d_set_cpu_flags_mask` was written and removed once measurement showed the dispatch is a cascade of overwrites, so an AVX2 build runs SSE code for every function with no AVX2 version whatever the mask says -- leaving the call a no-op on the avx2 build and a slowdown on the baseline one. CI had neither meson nor nasm, so the module was being skipped in every leg with the build still green -- both now come from the image's Miniconda, in a step before the developer environment and followed by a version report, because a silently skipped decoder looks exactly like a passing build |
| M6.0 | AV1 crosses the container | **done, and it is half of `codec_dav1d`.** MP_CODEC_AV1 appended, `demux_mp4` reading an `av01` sample entry, and the `av1C` record handed over verbatim -- read by asking the box to write itself, because Bento4 parses this one and keeps no raw bytes, and a record reassembled from parsed fields would be this module's opinion of the file rather than the file. `av1.mp4` is `av.mp4`'s picture and audio in AV1, differing in the codec and in nothing else. The decoder waits on a build decision rather than on code: dav1d is Meson-only, every submodule here is CMake, and the machine's only Python has no pip |
| M5.99 | The presenter draws every shape v4 can describe | **done.** 4:0:0, 4:2:0, 4:2:2 and 4:4:4, planar or semi-planar, at 8 through 16 bits, with one matrix and one transfer that a two-plane and a three-plane entry point both reach. The subsamplings need no case: normalised coordinates make a half-width chroma plane and a full-width one the same call, so the general form is less code than the 4:2:0 special case it replaced. Two things the tests found rather than the reasoning: 4:0:0 fails as a strong green rather than as an error, because an unbound texture samples to zero and zero is not neutral chroma; and neutral chroma is 127.5, not 128, because half the full scale falls between two codes at every even depth -- the shader was right and the first test was not. Interleaved Y'CbCr is refused with a sentence |
| M5.98 | ABI v4: a frame describes its pixels | **done.** MpPixelFormat was six DXGI names in a header meant to outlive Direct3D, and could not say 4:2:2, 4:4:4 or twelve bits at all -- while naming the combinations would have taken seventy-five enumerators. MpPixelLayout is six fields and the arithmetic over them, and `shift` is what earned the break: ten bits at the top of sixteen and ten at the bottom are the same depth and a factor of sixty-four apart, which v3's `bool ten_bit` had no way to be right about. The general formula reproduces the constant `yuv_matrix.cpp` carried, exactly, which is what makes it a refactor. Doing it before `codec_dav1d` rather than after cost one producer and one consumer, and turned up a ten-bit frame `codec_mft` had been labelling eight |
| M5.97 | Section 9.9: a video packet says what its timestamp is counted in | **done.** MpVideoInfo::timescale, appended -- the first time the size prefix earned its keep, and a test asks for the older size to check it. Answering it turned up three more: MP4 was reporting decode timestamps where Matroska reports presentation ones, MP_PACKET_SYNC was claimed on every packet including video, and the edit list was read for audio tracks only -- sixty milliseconds of A/V offset in the fixture. A video seek landed one frame late, so it subtracts the track's largest composition offset before a lookup that indexes decode time -- with a guard for Bento4 reading that offset unsigned |
| M6.10 | The display loop, and a window with a picture in it | **done.** DisplayLoop in the engine and two clocks in the head: IFrameClock answers when a frame may be drawn and what time it is, in one object because they are one clock -- the tick a frame is drawn at is the tick the audio position is extrapolated to. VBlankClock waits on the output the window is actually on rather than the adapter's first, TickClock is the fallback, and the loop runs on its own thread because WaitForVBlank blocks a whole refresh and a message queue nobody drains for sixteen milliseconds is an unresponsive window. It re-reads the graph's clock_spec every turn and reconfigures when a seek moved the anchor, and a device that stops answering keeps its last reading rather than blanking the picture. `mediaperch-probe show` is the whole of it wired up -- one demuxer, the router feeding both halves, the audio graph that owns the clock, a window, and the loop -- which is §9.7.1's one-process-one-window case and is deliberately not in the engine. Measured on this machine with all four decoders: 24 shown and none dropped through codec_mft, dav1d and libvpx, 16 through avm, sixty turns for a one-second file on a 60 Hz display, and every worst-late figure inside one refresh, which is the floor. --no-audio pages in a wall clock for a file with no audio track and says so, because §8's clock is the audio device and there was none -- this machine's endpoint refused every format while that was measured |
| M6.9 | One demuxer, and the position two consumers share | **done.** PacketRouter reads one demuxer once and hands each selected stream an IPacketFeed, which fills the hole VideoGraph was written around without changing a line of it. The ABI header already said what is wrong with the alternative: two file positions that a seek has to move separately and land on the same moment. So seek here is one call that moves the file and empties every queue, because a seek that left them would hand a consumer packets from before it. The queues are the only buffering and most packets miss them -- a consumer asking for its own stream has the demuxer read straight into its own buffer, serving a queued packet is a vector swap, and the vectors are recycled so a 4K keyframe costs no allocation. A per-stream cap answers MP_ERR_BUSY rather than growing, because dropping would be silent corruption and blocking would be a deadlock between two threads; VideoGraph reads that as its repeated, which is what it already does when nothing is due. Checked by equivalence: what each stream gets through the router is what it would have got from a demuxer of its own, packet for packet and byte for byte, with one file position. Two assertions that first test made were wrong and both were the fixture -- av.mp4's whole audio track is four kilobytes in forty-four packets, so a four-kilobyte cap fills only after the file ends, and a seek to half a second lands on frame zero because that fixture's only sync sample is its first |
| M6.8 | The video graph: decode, pace, present | **done.** VideoDecoder and Presenter behind their vtables -- mp::Sink for pictures -- and VideoGraph, which holds one frame, asks §8's pacer and presents. One frame and no queue, because a decoded frame is valid until the next call on the codec that produced it and a queue would have to copy what §9.8.1 went to some trouble not to copy; the lookahead is inside the decoder, which reorders B-frames and since M6.6 uses every core. No thread of its own either: the audio graphs own one because the device's event paces them, and video's pace is the display's, which belongs to the head. A drop does not cost a refresh -- one pump lets go of every frame whose time has passed, because letting one go per refresh would never catch the clock. After the first frame the decoder is asked what it actually produced and the presenter reconfigured where the bitstream disagrees with the container, except for the timescale and the frame rate, which a decoder never re-times. Packets arrive through IPacketFeed rather than from a demuxer, which is a hole with a name: §4 says one file has one position, so audio and video must share one demuxer, and the router that would do that is what comes next. Checked on demux_mp4 + codec_dav1d + video_d3d11 with a clock somebody chose: 24 shown and none dropped at the right speed with nothing more than a millisecond late, twelve dropped and twelve shown half a second behind with the picture still right at the end, and five hundred polls of a stopped clock holding it |
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

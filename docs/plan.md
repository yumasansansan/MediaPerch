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
| Toolchains | MSVC and clang-cl on Windows; Clang on Linux when there is a Linux head. **GCC is not supported anywhere** — a third warning dialect and a third set of quirks for a platform Clang already covers — and configuration refuses it rather than drifting into it |
| Layer | as low as practical. Prefer the platform API over a wrapper when the wrapper adds no capability we need |
| Audio | WASAPI **exclusive**, event-driven, MMCSS `Pro Audio`. Shared mode is a fallback, not the design centre |
| Bit-exactness | a testable property, not a marketing word. §12 says how it is tested |
| Video | Direct3D 11 + DirectComposition. HDR delegated to **the OS tone mappers** by default — with a correct one selectable, because the OS one is known to be wrong (§9.2) |
| Modularity | decoders, sinks, DSP and the video presenter are runtime-loaded shared libraries behind one C ABI |
| Shell | separate process, optional, replaceable. The engine is complete without it |
| Windows floor | Windows 10 2004 for audio; Windows 11 22H2 for Advanced Color; Windows 11 24H2 for the desktop HDR-state APIs, degrading gracefully below each |
| IDE | Visual Studio 2026, opened as a real `.sln`, as with DragonPerch |
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
- The safety gap is closed the way DragonPerch already closes it: libFuzzer on every parser,
  ASan/UBSan in CI, `/GS` and `/guard:cf` in release. That machinery exists and the
  maintainer already runs it.
- For one maintainer, the cost that does not appear in a CI log — switching between two
  languages, two dependency ecosystems, two fuzzing setups — is the one that actually bites.

### The decision

> **C++23 for `src/core`, the platform heads and every module. A plain C module ABI, kept
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

Any of those, and `decode_native` becomes the first Rust module. Until then the ABI is ready
and the toolchain is one.

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
| `MP_KIND_DSP` | `negotiate`, `process` (`MP_RT`), `reset` | f32 deinterleaved in and out; latency declared, not measured |
| `MP_KIND_VIDEO` | `create_surface`, `present`, `set_colour_target` | §9 |
| `MP_KIND_META` | `read_tags`, `read_art` (`MP_IO`) | the most hostile input in the program; fuzzed hardest |

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
  decoder ─▶ f32 deinterleaved bus ─▶ DSP chain ─▶ requantise + dither ─▶ ring ─▶ sink
```

Entered when the user asks for DSP, when a resample is unavoidable, or when negotiation
failed and the user chose to convert rather than not play. The canonical bus is f32
deinterleaved because every DSP anyone will write wants it that way, and one conversion at
each end is cheaper than N conversions inside.

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
It lives in `src/core` and is one of the two things `tests/` cares most about.

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
component. For a `GPL-3.0-or-later` project it is now simply a module — `sink_asio`, someday,
behind the same vtable as `sink_wasapi`, chosen by the user and absent from the default
install.

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
| **Convert** | fall to Path B with the best available resampler. Loud in the UI about what it did |
| **Shared** | fall to shared mode, ideally with `AUDCLNT_STREAMOPTIONS_RAW` via `IAudioClient2::SetClientProperties` to bypass system effects, and `IAudioClient3::InitializeSharedAudioStream` at `GetSharedModeEnginePeriod` for latency |
| **Refuse** | do not play, and say exactly which format the device declined |

"Refuse" must exist. It is the option that makes the other two honest.

---

## 7. Decoders, and how one is chosen

| Module | Backend | Covers | Why it exists |
|---|---|---|---|
| `decode_native` | C++, `dr_flac` and `dr_wav` single headers | FLAC, WAV | the floor: no build system beyond two `#include`s, so an install with nothing else on disk still plays music. **Measured bit-exact** for 16- and 24-bit, and measurably *not* able to read 32-bit FLAC |
| `decode_flac` | libFLAC, the Xiph reference decoder, as a submodule | FLAC, all depths and rates | for a lossless codec the reference implementation *is* the specification, which is worth a dependency in a way it would not be for a lossy one. Reads what `dr_flac` cannot, and checks its own output against the MD5 the encoder wrote into the file |
| `decode_mf` | Media Foundation `IMFSourceReader` | MP4/M4A, AAC, MP3, WMA — and WAV and FLAC, **also bit-exact** | ships with Windows, hardware-accelerated, and it is what brings §9 for free. Scores itself below `decode_native` on WAV and FLAC because it reaches them through a pipeline that *could* insert a converter, not because it did |
| `decode_ffmpeg` | libav* | everything else — and **32-bit FLAC**, which neither of the others can read | the long tail, and the first candidate for out-of-process hosting. Measured: FFmpeg decodes a 32-bit FLAC byte-identically to the reference decoder, which is the first concrete reason to build this rather than a general appeal to coverage |

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
than naming the one it wants. Measured: FLAC and WAV go to `decode_native` (100 against
Media Foundation's 40), MP3 and M4A go to `decode_mf` (100 against nothing), and forcing
`--decoder native` on an MP3 produces a clean refusal rather than a guess.

The choice is per-track and visible: the UI and the log both name the module that opened
the file.

### Where dependencies come from

Two mechanisms, and which one a dependency gets is decided by whether **we** build it.

| | Examples | Why |
|---|---|---|
| **Git submodule, built from source** | `external/dr_libs`, `external/flac` | Pinned to a commit by the gitlink, so a checkout is reproducible and an upgrade is a reviewable diff. Both have (or need) no build system of consequence: dr_libs is headers, libFLAC is CMake-native. The tree builds them; CI builds them; nothing is downloaded at configure time except Catch2 |
| **Loaded at run time, never vendored** | FFmpeg, one day | Its configure is a shell script needing MSYS2 and nasm on Windows, its build is tens of minutes, its output is tens of megabytes, and **its licence is a choice the user should make** — LGPL-2.1+ by default, GPL with `--enable-gpl`, and non-free options past that. Vendoring one configuration decides all of that for them. A module that resolves `avcodec` at run time and reports itself unavailable when it is absent respects every one of those and costs the build nothing |

The rule generalises: **vendor what you compile, resolve what you don't.** A module that
cannot find its dependency simply does not load, which is the behaviour the whole
architecture already has for a module that is not installed.

On "should every decoder use the official library" — no, and the reason is not size. For a
**lossless** codec the reference implementation is the specification, and a reimplementation
can drift from it silently: §14 records `dr_flac` decoding a 32-bit FLAC to nothing at all.
For a **lossy** codec "correct" is a tolerance rather than an identity, so the argument is
much weaker, and for AAC the obvious candidate (FDK-AAC) carries a licence that is not
GPL-compatible at all. WAV has no reference implementation to prefer, because it has no
reference implementation. So: official libraries for lossless codecs, whatever decodes well
for lossy ones, and the module boundary so the choice stays with whoever installs it.

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

Present the OS path first (`driver` tone mapping, `decode_mf` hardware decode, flip-model
scRGB) and get it correct. Only then add `shader`, and only as an option — a hand-written
tone mapper that ships before the platform one has been made to work is how a project ends
up maintaining a colour pipeline it never meant to own.

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
| decoders | against a reference. `mediaperch-probe decode --file X` prints SHA-256 of the PCM; `ffmpeg -i X -f s16le out.raw` prints the same thing if the decoder is right. Measured: WAV and FLAC, 16-bit and 24-bit, all four hashes identical to FFmpeg's |
| ring | a soak test with a producer and a consumer under TSan, plus an assertion that the render side never allocates (an allocator hook that aborts while the RT flag is set) |
| parsers | libFuzzer on every one, with corpora in `fuzz/corpus`, as DragonPerch already does. `dr_wav` and `dr_flac` have targets and **run in CI on every push** — thirty seconds each, which is a smoke test that the campaign still builds rather than a campaign, and somewhere for a regression corpus to live. §2 chose C++ for the parsers on the argument that fuzzing closes the gap; an unrun fuzzer would have made that argument worthless |
| properties | randomised invariants over the whole format space, in `tests/`, with a fixed-seed generator so a failure prints a seed that reproduces it. About 15,000 cases per run, no hardware, no Clang: every candidate list is bit-exact and free of duplicate wire formats, non-PCM encodings are never repacked, and `repack` round-trips for every container pair that fits and refuses every pair that does not. This is the cheap half of fuzzing, and it runs on both compilers |
| devices | a manual matrix, because it cannot be automated, kept in [devices.md](devices.md): onboard codec, a USB DAC, HDMI to a receiver, Bluetooth. For each, the formats that negotiated, whether `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` appeared, and the minimum period that ran glitch-free for an hour |
| glitch counting | the engine counts underruns, `AUDCLNT_E_DEVICE_INVALIDATED`, and late render callbacks, and shows them. A player that cannot tell you it glitched cannot be trusted when it says it did not |

---

## 13. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | Repository skeleton, CMake presets, CI | `core` builds alone with the platform directories off the include path, and CI fails if that stops being true |
| M1 | WASAPI exclusive, event-driven, sine from memory | a 1 kHz tone plays for an hour at the minimum device period with zero underruns; the realign path in §14 is exercised deliberately |
| M2 | Module ABI v1 + `decode_native` + `sink_capture` + the two throwaway ABI probes (§2) | **mostly done.** `decode_native` (WAV and FLAC, `dr_wav`/`dr_flac`) decodes to hashes identical to FFmpeg's; the fake sink in `tests/` and the tee in `verify` both prove the bytes reach the device unaltered. The C and Rust ABI probes are still to write |
| M3 | `mediaperch-cli` and the IPC | the engine is genuinely usable with no GUI on disk |
| M4 | Path B: f32 bus, DSP chain, resampler, dither. Gapless, seek | switching paths at a rebuild point is glitch-free and the passthrough path still contains no float |
| M5 | `decode_mf` and `decode_ffmpeg`, and the resolution table | the format coverage matrix in the README is real and generated by a test |
| M6 | Video: D3D11, DirectComposition, hardware decode, A/V sync off the audio clock | 4K HEVC plays with frames dropped against audio, never the reverse |
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
| Memory-safety bugs in parsers, now that Rust is not doing that job | libFuzzer on every parser from M2, ASan/UBSan in CI, `/GS` and `/guard:cf` in release, and `decode_ffmpeg` out of process — which is the only mitigation that covers FFmpeg's own surface, and Rust never would have |
| The video half quietly becomes the whole project | audio is complete and shippable at M5. Video is M6 onward and is allowed to be late |
| FFmpeg's licence and binary size make it awkward to ship | it is a module, so ship it separately; `decode_native` and `decode_mf` mean the base install still plays music and video |

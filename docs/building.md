<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Building MediaPerch

## What you need

| | |
|---|---|
| Windows | 10 version 2004 or later. Windows 11 24H2 for the HDR paths, when they exist |
| Compiler | Visual Studio 2026 (MSVC 19.51 or later) on Windows; LLVM Clang, GNU driver, for the fuzzers and for Linux when there is a Linux head. **Neither GCC nor clang-cl is supported** — configuration fails on purpose for both |
| CMake | 3.28 or later |
| Ninja | for every preset except `vs` |

## Checking out

The tree uses submodules for everything it compiles that is not ours:

```bash
git clone --recurse-submodules https://github.com/yumasansansan/MediaPerch.git
```

or, in a clone that already exists:

```bash
git submodule update --init
```

| Submodule | What for | Which module brings it in |
|---|---|---|
| `external/dr_libs` | `dr_wav` | `demux_wav` |
| `external/flac` | libFLAC | `codec_flac` |
| `external/ogg` | libogg | `demux_ogg` |
| `external/vorbis` | libvorbis | `codec_vorbis` |
| `external/opus` | libopus | `codec_opus` |
| `external/libebml` | EBML, the encoding Matroska is written in | `demux_mkv` |
| `external/libmatroska` | the Matroska schema on top of it | `demux_mkv` |
| `external/utfcpp` | header-only UTF-8, which libebml asks for by `find_package` | `demux_mkv`, through libebml |
| `external/Bento4` | ISOBMFF: MP4, M4A, `.mov`, fragmented MP4 | `demux_mp4` |
| `external/mpg123` | MPEG audio layers I to III, both halves | `demux_mpa`, `codec_mpa` |
| `external/wavpack` | WavPack, lossless and lossy and DSD | `demux_wavpack` |
| `external/asio` | the ASIO interface, headers only | `sink_asio` |
| `external/vst3sdk/pluginterfaces` | the VST3 interfaces, and four .cpp files | `dsp_vst3` |

**Each submodule sits with the one module that needs it**, which is a property of the
container/codec split rather than a tidying. One module used to bring in four of the Xiph
libraries at once, because it was the container and both codecs together. libogg belongs to the
container reader, and libFLAC is asked for two different things by two different modules:
`codec_flac` decodes with it, and `demux_flac` asks it only where frames begin and end.

Two orderings survive, both for the same reason -- a library asking
`find_package` for one this build has already compiled, answered by a shim in
`cmake/`:

- libvorbis asks for `Ogg`, so `modules/demux/ogg` comes before
  `modules/codec/vorbis`.
- libmatroska asks for `EBML`, so `modules/demux/mkv` adds `external/libebml`
  before `external/libmatroska`.

And one that is not a `find_package`: `modules/codec/flac` brings in libFLAC and
`modules/demux/flac` links the same target, so the codec comes first.

### Five small CMake overrides, and why they are in `cmake/`

The Xiph libraries are built in-tree, which their own CMake does not quite expect. Five
files in `cmake/` fix that, all by the same mechanism: every one of those projects reaches
its own modules with `list(APPEND CMAKE_MODULE_PATH ...)`, and this project's `cmake/` is
already on that path from the top-level `CMakeLists.txt`, so ours is found first.

| File | What it replaces |
|---|---|
| `FindOgg.cmake` | libvorbis calling `find_package(Ogg REQUIRED)` for an *installed* libogg. Ours answers with the in-tree `Ogg::ogg` target, which `modules/demux/ogg` creates first |
| `FindEBML.cmake` | libmatroska calling `find_package(EBML 2.0.0)` for an *installed* libebml. The same trick, plus the `EBML::ebml` alias libebml does not declare for itself |
| `Findutf8cpp.cmake` | libebml calling `find_package(utf8cpp)` -- and, finding none, **downloading one from GitHub at configure time** with FetchContent. That had been happening on every configure, locally and in CI, with a warning as the only sign. utfcpp is a submodule now, at the tag libebml would have fetched, and this answers with it |
| `OpusPackageVersion.cmake` | opus asking `git describe --tags` for its version number |

The second matters more than it looks. Upstream has **no working fallback** when git cannot
answer: `configure.ac` carries the literal placeholder `CURRENT_VERSION` that their release
script fills in, and no `package_version` file is committed. In a checkout without tags --
a CI runner fetching submodules at depth 1, a source archive, a `git clone --depth 1` --
the describe fails and the version becomes `0`. While `opusfile` was in the tree that was a
hard configure error rather than a cosmetic one: its

```cmake
list(GET PROJECT_VERSION_LIST 1 PROJECT_VERSION_MINOR)
```

failed with `list index: 1 out of range`. That is exactly what CI hit, and it did not
reproduce locally only because a full clone has the tags. The version is now pinned beside
the `add_subdirectory` call in `modules/codec/opus/CMakeLists.txt`, so the number and the
gitlink move together, and `cmake -D CMAKE_DISABLE_FIND_PACKAGE_Git=ON` configures cleanly.

**`opusfile` is no longer here at all.** It is the Ogg container and the Opus codec in one
object, which is what `decode_ogg` used; `demux_ogg` reads the container and `codec_opus`
drives libopus directly, so nothing was left calling it. Two of the shims above went with
it, and so did `OP_DISABLE_HTTP` -- opusfile can fetch a stream over the network, which a
local file player has no use for and which was a setting somebody had to remember rather
than a thing that was absent.

`external/vorbis` is pinned to upstream `master` rather than to a release tag: v1.3.7
declares `cmake_minimum_required(VERSION 2.8.12)`, which CMake 4 refuses outright, and
upstream has fixed it on `master`. A submodule pins an exact commit either way, so the
checkout is still reproducible; what is given up is a version number, not determinism.

`codec_alac` and `codec_aac` are on none of these lists on purpose: the ALAC and AAC-LC
codecs are in this tree, as are the MP4 and ADTS container readers, so those four modules
build from a checkout with no submodules at all.

A missing submodule is not a build failure: the module that wanted it prints a warning and
skips itself, and the rest of the tree builds. What that costs is per-codec now rather than
per-format -- without `external/flac` there is no `codec_flac`, but `demux_flac` still reads
the container and says which codec it could not find. Catch2 is fetched at configure time instead,
because it is test scaffolding rather than something that ships. Nothing else is
downloaded.

**FFmpeg is not a build dependency at all.** `demux_ffmpeg` looks for `ffmpeg` and
`ffprobe` beside itself and then on `PATH`, at run time, and declines every file when
neither is there. Installing one is optional and is the user's choice — including the
choice between an LGPL and a GPL build.

## The presets

**From a developer prompt** — `VC\Auxiliary\Build\vcvars64.bat`, once per shell.
Every preset here is Ninja, and Ninja does not go looking for Visual Studio; the
section below is about what happens when you forget.

```bash
cmake --preset ninja-msvc && cmake --build --preset ninja-msvc-release && ctest --preset ninja-msvc-release
```

**Release, not Debug, unless you are debugging.** The decoders do real arithmetic
on real amounts of audio, and a Debug build is five to ten times slower at it:
the whole test suite takes **177 seconds in Debug and 41 in Release**, and the
decode-quality check inside it goes from 174 to 39.

| Preset | Toolchain | For |
|---|---|---|
| `ninja-msvc` | MSVC | day to day. `-debug`, `-release` and `-relwithdebinfo` build presets |
| `measure` | MSVC | Release **with the measuring apparatus kept** — see below |
| `core-only` | MSVC | what CI builds to keep `src/engine` and `src/player` portable, and the engine free of the player |
| `asan` | Clang | the parsers under ASan and UBSan |
| `fuzz` | Clang | the libFuzzer targets |

**There is one generator, and it is Ninja.** A Visual Studio generator was here
and is not any more, because keeping both meant two of everything: two build
trees, two sets of build and test presets, and a `compile_commands.json` that
existed in one of them and not the other — which `.clang-tidy` and clangd both
need. The fuzzers and the sanitized build settle the question on their own: both
drive Clang's GNU driver with libFuzzer, which no Visual Studio generator can do,
so Ninja was never removable and the only choice was whether to keep a second
one. Visual Studio opens this tree with **File ▸ Open ▸ Folder**, which reads
`CMakePresets.json` directly and debugs the same binaries.

## What a Release build leaves out

A build that ships has no test suite and no measuring apparatus in it. The
apparatus is `mediaperch-probe compare` and `mediaperch-probe verify` — how every
hard bug in this project was found, and not one byte of it needed to play a
file. Leaving it out is **46 KB of a 169 KB probe**, most of it the analysis in
`compare.obj` and `verify.obj`, which live in static libraries and are simply
never linked once nothing calls them.

The libraries are still built and still unit-tested in every configuration. It
is only the executable that does without, so the code cannot rot:

| | `compare` and `verify` | Why |
|---|---|---|
| Debug | present | it is what you are there for |
| Release | **absent** | it is not what ships |
| Release, `-D MEDIAPERCH_DIAGNOSTICS=ON` | present | the `measure` preset is exactly this |

Asking a build without them for one says so and exits 77 — the code a test
runner reads as *skipped* rather than *failed*, so `ctest` on a shipping build
reports the decode-quality check as not run instead of pretending it passed.

```bash
cmake --preset measure && cmake --build --preset measure && ctest --preset measure
```

## Optimisation, and link-time optimisation

Every flag is in `cmake/CompilerOptions.cmake`, and the file is arranged around
one distinction:

- **Policy** — the warning dialect, the exception model — attaches to the
  `mediaperch_flags` interface target, which only this project's own targets
  link. Imposing `/W4` on libvorbis fails a build that has nothing wrong with it.
- **Optimisation** goes through `add_compile_options` at directory scope, so it
  reaches everything the build compiles, submodules included. libFLAC, libvorbis
  and libopus are most of the bytes that ship; optimising only the tenth of the
  binary this project wrote would be a strange place to stop.

**An optimised build takes every optimisation there is, and there is no option
to take fewer.** A per-flag switch is a promise to keep every combination of
them working, and the only combination anybody ships is the one where they are
all on; the rest would be untested configurations wearing the same name. So
Release is `/O2 /Oi /Ot /Gy /Gw /Ob3` with `/OPT:REF /OPT:ICF` and link-time
optimisation, and asking for less means editing the file.

**Except anything that trades accuracy for speed.** `/fp:fast` is the obvious
one, and it is deliberately absent: it lets the compiler reassociate floating
point, which is precisely the transformation the measurements in
[formats.md](formats.md) exist to prove did not happen. Speed that costs a digit
is not speed this project wants.

**A Release build prints `D9025: '/Ob3' takes precedence over '/Ob2'` once per
file of libFLAC, and that is correct.** libFLAC's own CMakeLists prepends
`/O2 /Ob2 /Oi /Ot /Oy` to the Release flags; `/Ob3` is added after them and
wins, which is the intent. Silencing it would mean either patching a submodule
or inlining less.

Link-time optimisation is not a switch either: it is part of what Release *is*.
The two builds that do without it are the sanitized one and the fuzzers, which
exist to observe the program rather than to be fast -- cross-module inlining
moves the frames a sanitizer report and a fuzzer crash both point at. Everything
else gets it, and **a toolchain that cannot do it stops the configure** rather
than quietly building something else: a Release binary without LTO is not the
binary this project measures, and the difference would arrive months later
looking like an unexplained regression.

Three options remain, and none of them changes the arithmetic:

| Option | Default | What it does |
|---|---|---|
| `MEDIAPERCH_DIAGNOSTICS` | OFF | keep the measuring commands in an optimised build |
| `MEDIAPERCH_LINK_MAP` | OFF | a `.map` beside every binary, for `tools/mapsize.py` |
| `MEDIAPERCH_SANITIZE` | OFF | ASan and UBSan, and no LTO with them |

`cmake/CompilerOptions.cmake` is one block per toolchain, and there are two:
**MSVC** on Windows and **Clang's GNU driver** everywhere else. Each lists its
own warnings, hardening and optimisation in full, and they share no spelling.

That is why **clang-cl is refused rather than supported**. It is Clang wearing
MSVC's words and meaning different things by several of them — `/Ob3` maps to a
different inliner, `/Zc:preprocessor` is a no-op it warns about, MSVC warning
numbers name nothing — so every flag in the file needed a second reading to work
out which compilers it reached. Two toolchains that share nothing are simpler
than three that share most things, and Linux arrives with a real Clang anyway.

## Rust, for the modules that are Rust

Three modules are Rust -- `codec_alac`, `codec_aac` and `demux_adts` -- and
the way they are built is written down here rather than left in
`cmake/Rust.cmake`'s comments alone.

**The toolchain is stable, from rustup, on the MSVC target.** Nothing here needs
nightly, including the fuzzer (below). `cargo` has to be on PATH when CMake
configures; if it is not, the modules are skipped with a warning the way a
missing submodule is, and ALAC, AAC-LC and raw ADTS fall to the next reader.

**Nothing links across the language boundary.** A module is a `.dll` on disk
that exports `mp_module_entry`, and the host cannot tell which compiler made
it. So `mediaperch_add_rust_module` is a `cargo build` into
`<build>/cargo/` and a copy to `bin/<config>/modules/<kind>/` under the same
name the C++ layout would give it -- no Corrosion, no CRT matching, no change
to a single C++ flag. The cargo profile follows the CMake configuration: Debug
builds the dev profile, with overflow checks on; every other configuration
builds `--release`, with fat LTO and one codegen unit, set once in
`modules/Cargo.toml`.

**One workspace, no dependencies.** `modules/Cargo.toml` is a virtual workspace
over every Rust crate in the tree: `shared/mp-abi`, which is the C ABI crossed
once and the only place a module's `unsafe` lives; a decoder or framer crate
per module -- `codec/alac/decoder`, `codec/aac/decoder`, `demux/adts/framer`
-- each `#![forbid(unsafe_code)]` at crate level; and the module crates,
`codec/alac`, `codec/aac` and `demux/adts`, the glue that implements
`mp_abi::Codec` or `mp_abi::Demux` and is the `.dll`. None of them depends on
anything outside the tree, so the lock file is small and a build needs no
network. The tests live in the decoder crates, and ctest runs
`cargo test --workspace` as one entry, `rust_modules`.

**The fuzzer is coverage-guided on stable, which took finding.** `cargo-fuzz`
wants nightly for `-Zsanitizer`, but the coverage half of what it does is
LLVM's SanitizerCoverage pass, which stable `rustc` reaches through
`-C passes=sancov-module` and a few `-C llvm-args`; `libfuzzer-sys` supplies
the `__sanitizer_cov_*` symbols those emit. Five things bit on the way, four
of them as a link error first:

- `RUSTFLAGS` reach cargo's *build scripts* too unless `--target` is spelled
  out, and a build script instrumented for sancov references symbols nothing
  provides. Measured: `getrandom`'s build script failed to link, LNK1120.
  `fuzz/CMakeLists.txt` passes `--target <host triple>`.
- A crate that is both `cdylib` and `rlib` gets its `cdylib` built when it is a
  dependency, and the `.dll` then needs the sancov symbols too. That is why the
  decoder is its own `rlib` crate and the module crate is `cdylib` only.
- libFuzzer's `main` lives inside an archive, and `link.exe` will not infer an
  entry point from a library member: LNK1561. `-C link-arg=/ENTRY:mainCRTStartup`
  names it, and the CRT's reference to `main` pulls it in.
- The coverage counters live in a section whose start and end symbols an ELF
  linker synthesises and a COFF linker does not; clang gets them from
  compiler-rt's sanitizer runtime, which `libfuzzer-sys` does not carry.
  `modules/shared/fuzz/sancov_sections.c` is that runtime's one relevant
  page -- six sentinels in `$A`/`$Z`-suffixed sections -- built by each fuzz
  crate's `build.rs` on the MSVC target only. Measured: LNK2001 on
  `__start___sancov_cntrs` from every instrumented object.
- With more than one codegen unit, LLVM's coverage pass died on the AAC
  decoder with `Associative COMDAT symbol ... does not exist`: it hangs a
  counter section off each function's COMDAT, and a generic instantiation one
  unit references and another later drops leaves a section pointing at
  nothing. Five flag combinations were tried; `-C codegen-units=1` is the one
  that links, and it is set for every Rust fuzzer. It costs a slower build and
  nothing at run time.

The fuzz crates are outside the workspace -- they are the only crates in the
tree with a crates.io dependency, and a module build must not depend on that
resolving.
AddressSanitizer is what stable cannot do; for safe Rust that is the smaller
loss, because the bounds checks are the sanitizer and a missed one is a panic
libFuzzer reports as a crash.

### The ASIO SDK, whose upstream is a ZIP

`external/asio` is the second submodule pinned to somebody else's copy, for the
same reason as mpg123 and with less to fall back on: **Steinberg publishes the
ASIO SDK as a download from their site and has no git repository at all.** They
put VST3 on GitHub and did not put ASIO there.

The pin is `audiosdk/asio`, at its only commit, which carries SDK 2.3.4 and the
licence file that matters. That file is the reason this is possible: *"This
Software Development Kit is licensed under the terms of the Steinberg ASIO
License, or alternatively under the terms of the General Public License (GPL)
Version 3"*, which Steinberg added in October 2025 and which a
`GPL-3.0-or-later` program may take.

Unlike the mpg123 pin, this one has **not** been diffed against the official
distribution, because the official distribution is behind a click-through and
nothing here can accept a licence on anybody's behalf. What can be said instead
is narrower and worth saying: **only headers are compiled**, and they are an
interface definition rather than an implementation -- `iasiodrv.h` is twenty-one
pure virtual functions and `asio.h` is enums and structs. There is no ASIO
library to link. If a header had been altered, what it would alter is a vtable
layout, and a wrong one does not play a tone through a real driver.

`asio.cpp` and `host/asiolist.cpp` are deliberately not compiled: the first keeps
one global driver pointer and the second enumerates the registry into fixed
`char[32]` buffers. `sink_asio` reads its own registry, for the same reason every
demuxer here opens its own files.

### The VST3 SDK, of which this takes one quarter

`external/vst3sdk/pluginterfaces` is `steinbergmedia/vst3_pluginterfaces` at
`v3.8.1_build_84`, and it is **the whole of what a host needs**: 690 kilobytes of
headers and four `.cpp` files, one of which exists to call `CoCreateGuid`.

The full SDK is four repositories. `pluginterfaces` is the contract;
`public.sdk` is a library of hosting and plugin helpers; `base` is a container
and string library from the 1990s; `vstgui` is a widget toolkit. A host normally
takes three of the four, for the module loader, the `HostApplication`, the
attribute list and the process-data plumbing -- roughly forty thousand lines to
do what `modules/dsp/vst3/vst3_host.cpp` and `vst3_hostapp.hpp` do in eight
hundred, and it arrives with a CMake package that wants opinions about the
build. So this tree writes those, and the one file it has to write to replace
`public.sdk` entirely is `vst3_iids.cpp`: a VST3 header *declares*
`static const FUID iid` and something has to define it.

**The licence is MIT since October 2025**, and that is the whole reason this
module exists. The previous VST3 licence was Steinberg's own, proprietary and
incompatible with GPLv3 in both directions; the same month they added GPLv3 as
an alternative to the ASIO licence, they relicensed VST3 outright. The copy here
carries `MIT License / Copyright (c) 2026, Steinberg Media Technologies GmbH`.

Unlike ASIO this pin *is* an official Steinberg repository at an official tag,
so there is no third-party mirror to justify and nothing to diff.

### mpg123, whose upstream is SVN

Every other submodule is pinned to a tag or a commit of the project's own git.
**mpg123 has no git at all** -- upstream is Subversion, at mpg123.org -- so the
pin is a mirror, and which mirror took checking rather than choosing:

| Candidate | State |
|---|---|
| `libsdl-org/mpg123` | newest *tag* is v1.30.0. Upstream is 1.33.7, and 1.33.7 fixes heap buffer overflows in Windows Unicode path handling. Its 1.33.x content is only on `-SDL` branches, which are a fork |
| `mileswu`, `georgi`, `wkpark`, `gypified`, `Bandwidth` | mirrors with **no tags at all** |
| **`madebr/mpg123`** | tracks SVN trunk, currently past 1.33.7 |

`madebr` is the one upstream itself uses: mpg123's own NEWS credits "github PR
23", "PR 29" and "PR 30", and those numbers are that repository's. The pin is
`c1d38c6`, which carries the 1.33.7 security fix and the bug 392 hardening after
it -- the same reasoning that pins Bento4 past its last tag.

**It was verified rather than trusted.** The official
`mpg123-1.33.7.tar.bz2` was downloaded from mpg123.de and its `src/` compared
against the mirror at the same day's commit. Twenty files differ: eleven are
ARM/NEON assembly this build never compiles, two are headers the tarball ships
generated, five are `libout123` and tests that `BUILD_LIBOUT123=OFF` excludes,
one is `version.h`. **Exactly one file that this build compiles differs --
`libmpg123/id3.c` -- and its difference is upstream commit `e1be2ba`, "store
UFID like TXXX (bug 384)", which postdates the release.** No commit is
byte-identical to a release tarball because the mirror linearises trunk and
releases are cut with packaging; that is expected of an SVN mirror, not evidence
of anything.

If that dependence is unwanted, the alternative is a mirror of your own:
`git svn clone` the upstream repository, push it to your account, and change one
URL in `.gitmodules`. It costs a periodic `git svn fetch && git push`.

**mpg123's own I/O is never used.** `mpg123_reader64` installs the read and seek
callbacks over a `FILE*` this tree opened with `open_utf8`, so the Windows path
conversion that 1.33.7 fixed is not reached at all -- the same arrangement
`demux_flac` has with libFLAC and `demux_mp4` with Bento4.

The fuzz build compiles libmpg123 a second time with
`-fsanitize=fuzzer-no-link,address`, for the reason the Rust ALAC target
records: `address` alone gives the sanitizer and no coverage counters, and
libFuzzer then walks the library blind. Measured here: `cov: 20` -- the harness
and nothing else -- over 289,000 executions before the flag was added, and 5,281
counters after.

One build note: `ports/cmake` sets its include paths with directory-scoped
`include_directories` rather than on the target, so `libmpg123` carries no usage
requirements and a consumer gets `fatal error C1083: mpg123.h`. The headers ship
in the source tree, so `modules/codec/mpa/CMakeLists.txt` puts one INTERFACE
path on the target and both modules inherit it.

### Two policy settings for the same submodules

CMake 4 warns, on every configure, about four submodules whose
`cmake_minimum_required` is 3.5 or 3.6: compatibility below 3.10 is going away.
The same declaration leaves policy CMP0069 unset in those projects, and CMP0069
unset means the `INTERPROCEDURAL_OPTIMIZATION` this tree sets for Release is
*ignored* for their targets -- six of them, each with a warning saying so, which
is how it was noticed. So libogg, libvorbis, libebml and libmatroska had been
building without the LTO the rest of the Release build has, since the day they
were added.

`CMAKE_POLICY_VERSION_MINIMUM 3.10`, set at the top level before any submodule
is added, gives those projects a policy version of 3.10: the deprecation warning
ends and CMP0069 becomes NEW, so they get the same LTO. `CMAKE_POLICY_DEFAULT_CMP0183
NEW` is for libebml's `add_feature_info()` calls, where NEW is the full condition
syntax and the ON/OFF values they pass mean the same under either. Seventeen
warnings per configure before; none after; every hash in the corpus the same
with LTO on.

## Two instruction-set baselines, both shipped

Everything this tree links already dispatches on the CPU: libFLAC, libmpg123,
libopus and libwavpack each compile several paths and pick one at run time. What
none of that covers is the code *here* -- the resampler, the convolver, the FFT,
the equaliser, the channel matrix, the dither -- which is Path B's inner loops
and compiles to the **x86-64 baseline, meaning SSE2 and 2003.**

Raising it is one flag and unshippable on its own: a binary built for AVX2 does
not start without AVX2. The usual answer is to compile the hot loops twice and
dispatch, which buys a dispatcher, a second copy of every stage, and a CPU check
on a path that must not branch. The answer here is to **build the whole tree
twice and upload both**, which costs a CI job and no code:

| | `MEDIAPERCH_ARCH` | Preset | Runs on |
|---|---|---|---|
| baseline | `baseline` | `ninja-msvc` | anything x86-64 |
| AVX2 | `avx2` | `ninja-msvc-avx2` | Haswell, Zen, and later |

`avx2` is x86-64-v3 -- AVX2, FMA, BMI1 and BMI2, LZCNT, MOVBE, F16C. MSVC spells
the set `/arch:AVX2` and Clang spells it `-march=x86-64-v3`.

**In the AVX2 build, libopus is told to stop checking.** It compiles an SSE, an
SSE2, an SSE4.1 and an AVX2 path and asks the CPU which to use; in a binary that
already refuses to start without AVX2 that question has one answer, and asking
it costs a branch and keeps three unreachable paths alive.
`OPUS_X86_PRESUME_SSE`, `_SSE2`, `_SSE4_1` and `_AVX2` are all on there and off
in the baseline. libFLAC and libmpg123 dispatch too and offer nothing to turn it
off with -- FLAC's is not an option and mpg123's `OPT_MULTI` is a local `set()`
in its own list file -- so their checks stay.

**Does it change the bytes?** FMA computes a multiply and an add with one
rounding where two instructions round twice, so it can. Measured across the
whole format corpus -- 22 files, every container and codec this tree reads,
DSD and WavPack included -- the two builds produce **identical hashes**.

That covered the decoders and not Path B, which is the half AVX2 was raised for,
and for a while there was no way to run the DSP chain without a device at all.
There is now -- `mp::Processor` is `ProcessedGraph`'s arithmetic with the device,
the ring and the threads taken out, and `mediaperch-probe decode` runs it. The
comparison is **144 runs: 12 files through 12 chains, including the resampler at
`quality=extreme`, both FFT modes of the equaliser, the channel matrix and two
noise shapers. Every one identical.** It is written out, with what it does and
does not say, under *Path B, hashed* in [formats.md](formats.md).

## Two assemblers, and what they were doing by accident

Both hand-written assembly paths in this tree were being taken or skipped for
reasons nobody chose, and a CI log is what showed it.

**libmpg123 needs `yasm` and does not say so.** Its CMake looks for one on PATH;
without it `MACHINE` silently becomes `generic` and the library loses OPT_MULTI,
OPT_X86_64 and OPT_AVX. Both machines this has run on found one -- `yasm.exe`
ships inside **Strawberry Perl**, which is on PATH here and pre-installed on
GitHub's Windows runners. Nobody asked for Perl and nothing declared it.

That would be a curiosity if the two decoders agreed. They do not: the same
2-second MP3 hashes `69dca145…` with the AVX synthesis and `f6c8a8e4…` with the
generic one. They agree to **127.84 dB**, maximum difference 1.5e-7, 10% of
samples identical -- float rounding between two implementations of one synthesis
filter, both correct by the RMS bound ISO 11172-4 calls conformance. But only one
is the hash [formats.md](formats.md) records. So `modules/codec/mpa/CMakeLists.txt`
looks for yasm itself and **warns, naming what a build without it will differ
by**, rather than letting a hash find it out later.

**libwavpack's assembly was decided by how many times you had configured.** Its
CMakeLists calls `enable_language(ASM_MASM)` guarded by `WavPack_CPU_X64` and
sets that variable ninety lines later, in the CPU detection -- which caches it.
So the first configure has the guard false and the assembly off, and the second
has it true and the assembly on, from identical source. Measured both ways.
`modules/demux/wavpack/CMakeLists.txt` enables the language itself before adding
the subdirectory, so the first configure is the same as the fiftieth.

There is **no reproducibility cost** to that one: WavPack is lossless, so its two
paths must agree or one is broken, and five files including a lossy hybrid one
hash identically with and without. The gain is 13%, and mpg123's is 9%.

**And MASM was being handed C++ flags.** `add_compile_options` reaches every
language in the directory, which for a tree of C and C++ was a distinction
without a difference until libwavpack arrived with `.asm` in it. `ml64.exe` was
given `/GS /guard:cf /guard:ehcont /Oi /Ot /Gy /Gw /Ob3` and failed with
`A1004: out of memory`, which is what that assembler says when it cannot parse
its command line and is as unhelpful a message as this build has produced. Every
global option in `cmake/CompilerOptions.cmake` now says
`$<COMPILE_LANGUAGE:C,CXX>`, which is what each of them always meant.

### `CMP0194`, and a Perl distribution holding up a configure

mpg123's `project(... LANGUAGES C ASM)` makes CMake look for an assembler, and
on Windows it settles for `cl.exe` and warns that MSVC is not one. It is right,
and it does not matter: nothing is assembled with `CMAKE_ASM_COMPILER` --
mpg123 spells `${CMAKE_C_COMPILER}` out where it preprocesses a `.S`, and yasm
does the assembling.

`NEW` is the worse answer and that took trying. With it CMake declines `cl.exe`
and keeps looking -- and on this machine it found `C:/Strawberry/c/bin/gcc.exe`
and was satisfied, which is a Perl distribution being load-bearing for a
configure a second time. On a machine with neither, `project(... ASM)` would
fail outright. `CMAKE_POLICY_DEFAULT_CMP0194 OLD` keeps the behaviour that works
anywhere and states the reason where the policy is set.

## What makes a binary big

The flags are settled and there is nothing left to tune there. What is left is
what the source asks for, and in C++ a single innocuous-looking call can pull in
more than the module around it.

| Instead of | Use | What it drags in |
|---|---|---|
| `std::format`, `std::to_string` | `std::to_chars`, `std::snprintf` | the shortest-round-trip float machinery — Ryu's tables and the locale facets around them — even when the argument is an `unsigned` |
| `<iostream>`, `<fstream>`, `<sstream>` | `std::fopen`/`std::fwrite`, and `mp::win::open_utf8` for a UTF-8 path | locale facets, the `std::ios_base::Init` static, and the stream buffer hierarchy, in every binary that touches a file |
| `std::filesystem::path` as a filename | `std::string` | `path`'s conversions, `error_code` and the directory machinery behind them |
| `std::regex` | a hand-written parser | more code than any parser in this tree |

Measured on the probe: replacing three `std::to_string` calls and the three
`std::ofstream`/`std::ifstream` uses took it from **123.5 KB to 115.0 KB**,
without changing a line of what it does. `mp::win::open_utf8` exists because the
narrow CRT calls go through the process code page and cannot open half the files
on a machine that is not English — the wide call can, and costs nothing.

`tools/gen_shaper_tables.py` is the other generator in the tree, alongside
`tools/gen_aac_tables.py`. It transcribes noise-shaping coefficients from
SSRC's `shapercoefs.h` and refuses to write a table whose entry count does not
match the source's — which is not hypothetical, it caught itself dropping two
of 67 on the first run. Neither generator runs during a build; both write files
that are committed, so no build needs Python or the network.

**`tools/mapsize.py` is how to find the next one.** Configure with
`-D MEDIAPERCH_LINK_MAP=ON` — off by default, because it writes a `.map` beside
every binary — and it charges every byte of a linked image to the object that
brought it:

```bash
python tools/mapsize.py --symbols build/vs/bin/Release/mp_codec_vorbis.map
```

**What that found.** The largest single object in the largest module is
libvorbis's `psy.obj`, 48 KB of psychoacoustic model — which only an *encoder*
uses, in a module that only decodes. It is there because `_vds_shared_init`
serves both directions and calls `_vp_psy_init` inside `if(encp)`: the branch
never runs in a decoder, the reference is unconditional, and **a linker keeps
what is referenced rather than what is reachable**. It brings `tonemasks` with
it, 22 KB of tables nothing will ever read. Removing it means patching a
submodule, so it stays — but it is measured and written down rather than filed
under "libvorbis is big".

## If it configures with the wrong compiler

It will not any more, and that is worth explaining because the failure it replaces
was silent.

**The Ninja generator does not go looking for Visual Studio.** It takes whatever `cc` and
`c++` are on `PATH`. Outside a developer prompt that is MinGW GCC on a GitHub runner,
Strawberry Perl's `gcc` or LLVM's `clang++` on a typical developer machine — and then the
whole project builds, cleanly, with nothing in the log admitting the toolchain was not the
one the preset is named after. **This is the price of having one generator**, and it is
paid with two guards rather than with a second build tree.

Two guards, because they catch different mistakes:

- **`MEDIAPERCH_EXPECT_TOOLSET`**, set by each preset that means a particular compiler.
  `ninja-msvc` configuring with Clang fails here even though Clang is a supported compiler.
- **The GCC rejection** in `cmake/CompilerOptions.cmake`, which fires however you got
  there, preset or not.

Either way the fix is the same: run `VC\Auxiliary\Build\vcvars64.bat` first. To build
with some other compiler deliberately, configure with `-D MEDIAPERCH_EXPECT_TOOLSET=`.

## Standards, and the one thing to know about them

`CMAKE_CXX_STANDARD 23` and `CMAKE_C_STANDARD 23` are set once at the top of
`CMakeLists.txt`. There is no `/std:c++23` and no `/std:c23` in MSVC — both are spelled
`latest` — and CMake already knows that, emitting `/std:c++latest` and `/std:clatest`.
Setting those flags by hand only earns a `D9025` for overriding what CMake put there first.

`include/mediaperch/module.h` is a deliberate exception: it stays inside the C11 common
subset of C and C++, because the whole point of that file is to be readable by a toolchain
we do not control. See §14 of [the plan](plan.md) for exactly which C23 features MSVC 19.51
has and which it does not.

## The three checks that are not unit tests

All three run as part of `ctest`, so they cannot be skipped by not remembering them.

- **`core_purity`** greps `src/engine` and `src/player` for OS headers and platform
  conditionals and fails the test run if either appears. `mediaperch_engine` is built alone
  in CI as well, so the rule is enforced from two directions. That second build checks a
  second rule for free: the engine target does not have `src/player` on its include path, so
  an engine file reaching for the transport or the playlist is a `C1083`, not a review
  comment.
- **`tests/abi_header_c.c`** is compiled as C rather than C++. The ABI header exists to be
  read by another language; a header that has only ever been through a C++ compiler has not
  been tested for that job. The `MP_STATIC_ASSERT` block in the header fires there under C's
  rules, so a layout disagreement between the two languages is a build failure here rather
  than a runtime surprise on somebody else's machine.
- **`decode_quality`** builds a dozen files with FFmpeg and holds every decoder against the
  uncompressed audio that was encoded: length, alignment, channel order, band energies and a
  fidelity floor, plus agreement with FFmpeg sample for sample. `docs/formats.md` has the
  numbers and the three deliberately-reintroduced bugs it was checked against.

  It is the one test here that needs a tool the build does not: **FFmpeg, on `PATH`**. Without
  one it skips itself and says so, which is right on a machine that has no FFmpeg and wrong in
  CI -- so CI configures with `-D MEDIAPERCH_REQUIRE_FFMPEG=ON` and a missing FFmpeg fails the
  job instead. FFmpeg is a *test tool* here and nothing else: nothing in the product links it,
  and `demux_ffmpeg` looks for it at run time.

  It takes a couple of minutes, so it carries a label:

  ```
  ctest --preset measure -LE quality          # everything except this
  ctest --preset measure -R decode_quality    # only this
  ```

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
| `external/dr_libs` | `dr_wav`, and `dr_mp3` | `demux_wav`, `codec_mp3` |
| `external/flac` | libFLAC | `codec_flac` |
| `external/ogg` | libogg | `demux_ogg` |
| `external/vorbis` | libvorbis | `codec_vorbis` |
| `external/opus` | libopus | `codec_opus` |

**Each submodule sits with the one module that needs it**, which is a property of the
container/codec split rather than a tidying. One module used to bring in four of the Xiph
libraries at once, because it was the container and both codecs together. libFLAC is now a
*codec* dependency and nothing else -- `demux_flac` reads the FLAC container with no library
at all -- and libogg belongs to the container reader.

The one ordering that survives: libvorbis asks `find_package(Ogg REQUIRED)`, so
`modules/demux/ogg` is added before `modules/codec/vorbis`.

### Three small CMake overrides, and why they are in `cmake/`

The Xiph libraries are built in-tree, which their own CMake does not quite expect. Four
files in `cmake/` fix that, all by the same mechanism: every one of those projects reaches
its own modules with `list(APPEND CMAKE_MODULE_PATH ...)`, and this project's `cmake/` is
already on that path from the top-level `CMakeLists.txt`, so ours is found first.

| File | What it replaces |
|---|---|
| `FindOgg.cmake` | libvorbis calling `find_package(Ogg REQUIRED)` for an *installed* libogg. Ours answers with the in-tree `Ogg::ogg` target, which `modules/demux/ogg` creates first |
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
| `core-only` | MSVC | what CI builds to keep `src/core` portable |
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

- **`core_purity`** greps `src/core` for OS headers and platform conditionals and fails the
  test run if either appears. `src/core` is built alone in CI as well, so the rule is
  enforced from two directions.
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

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Building MediaPerch

## What you need

| | |
|---|---|
| Windows | 10 version 2004 or later. Windows 11 24H2 for the HDR paths, when they exist |
| Compiler | Visual Studio 2026 (MSVC 19.51 or later), and LLVM/clang-cl as the second opinion. **GCC is not supported on any platform** — Linux will be Clang too — and configuration fails on purpose if it is used |
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

| Submodule | What for |
|---|---|
| `external/dr_libs` | `dr_wav` and `dr_flac`, behind `decode_native` |
| `external/flac` | libFLAC, behind `decode_flac` |

A missing submodule is not a build failure: `decode_flac` prints a warning and skips
itself, and the rest of the tree builds. Catch2 is fetched at configure time instead,
because it is test scaffolding rather than something that ships. Nothing else is
downloaded.

## The presets

```bash
cmake --preset vs && cmake --build --preset vs-debug && ctest --preset vs-debug
```

| Preset | Generator | For |
|---|---|---|
| `vs` | Visual Studio 2026 | day to day. Opens as a real `.sln`, needs no developer prompt |
| `ninja-msvc` | Ninja Multi-Config | faster, but run it from a developer prompt |
| `ninja-clang` | Ninja Multi-Config, clang-cl | the second compiler. Catches what MSVC lets through |
| `core-only` | Ninja Multi-Config | what CI builds to keep `src/core` portable |
| `asan` | Ninja Multi-Config, clang-cl | the address and undefined-behaviour sanitizers |

## If it configures with the wrong compiler

It will not any more, and that is worth explaining because the failure it replaces
was silent.

**The Ninja generator does not go looking for Visual Studio.** The `vs` generator finds
the toolset by itself; Ninja takes whatever `cc` and `c++` are on `PATH`. Outside a
developer prompt that is MinGW GCC on a GitHub runner, Strawberry Perl's `gcc` or LLVM's
`clang++` on a typical developer machine — and then the whole project builds, cleanly, with
nothing in the log admitting the toolchain was not the one the preset is named after.

Two guards, because they catch different mistakes:

- **`MEDIAPERCH_EXPECT_TOOLSET`**, set by each preset that means a particular compiler.
  `ninja-msvc` configuring with Clang fails here even though Clang is a supported compiler.
- **The GCC rejection** in `cmake/CompilerOptions.cmake`, which fires however you got
  there, preset or not.

Either way the fix is the same: run `VC\Auxiliary\Build\vcvars64.bat` first, or use the
`vs` preset, which needs no developer prompt at all. To build with some other compiler
deliberately, configure with `-D MEDIAPERCH_EXPECT_TOOLSET=`.

## Standards, and the one thing to know about them

`CMAKE_CXX_STANDARD 23` and `CMAKE_C_STANDARD 23` are set once at the top of
`CMakeLists.txt`. There is no `/std:c++23` and no `/std:c23` in MSVC — both are spelled
`latest` — and CMake already knows that, emitting `/std:c++latest` and `/std:clatest`.
Setting those flags by hand only earns a `D9025` for overriding what CMake put there first.

`include/mediaperch/module.h` is a deliberate exception: it stays inside the C11 common
subset of C and C++, because the whole point of that file is to be readable by a toolchain
we do not control. See §14 of [the plan](plan.md) for exactly which C23 features MSVC 19.51
has and which it does not.

## The two checks that are not unit tests

Both run as part of `ctest`, so they cannot be skipped by not remembering them.

- **`core_purity`** greps `src/core` for OS headers and platform conditionals and fails the
  test run if either appears. `src/core` is built alone in CI as well, so the rule is
  enforced from two directions.
- **`tests/abi_header_c.c`** is compiled as C rather than C++. The ABI header exists to be
  read by another language; a header that has only ever been through a C++ compiler has not
  been tested for that job. The `MP_STATIC_ASSERT` block in the header fires there under C's
  rules, so a layout disagreement between the two languages is a build failure here rather
  than a runtime surprise on somebody else's machine.

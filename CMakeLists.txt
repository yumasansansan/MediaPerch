# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.29)

project(MediaPerch
    VERSION 0.1.0
    DESCRIPTION "A bit-exact audio and video player for WASAPI exclusive mode"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# **The presets name every tool, and cmake/CompilerOptions.cmake holds a
# configure to them.** The toolchain is LLVM's on every platform -- Clang's GNU
# driver, LLD, llvm-ar -- and a configure that found anything else, MSVC or a
# MinGW GCC that happened to be on PATH, stops with a sentence saying what to do
# instead. That used to be a cache variable a preset could set; it is a rule
# now, because there is one toolchain to hold a build to.

option(MEDIAPERCH_BUILD_TESTS "Build the test suite" ON)
option(MEDIAPERCH_BUILD_FUZZERS "Build the libFuzzer targets" OFF)

# **The reference decoders' own validity checks, off by default.**
#
# libvpx can be told to check that intermediate transform coefficients stay in
# range, and libaom has neighbours of the same kind. They cost speed and they
# are exactly what a build being fuzzed or audited wants, so they are a switch
# rather than a preset: `asan`, a future decoder-fuzzing preset, or a person
# with a suspicious file can all turn them on, and nothing that ships has them.
#
# Deliberately not tied to MEDIAPERCH_BUILD_FUZZERS: that preset builds no
# modules at all -- MEDIAPERCH_BUILD_PLATFORM is OFF there -- so a decoder
# switch keyed on it would never reach a decoder.
option(MEDIAPERCH_DECODER_CHECKS
    "Build the reference decoders with their internal validity checks on" OFF)
option(MEDIAPERCH_BUILD_PLATFORM "Build the platform head and the modules" ON)
# The ABI probes from plan.md §2: a module in C, and one in Rust that cargo
# builds separately. Off because they are evidence rather than product -- see
# abi/README.md for what they measured.
option(MEDIAPERCH_BUILD_ABI_PROBES "Build the throwaway ABI probe modules" OFF)

# **C++23 and C23, asked for by name.** Clang is given `-std=c++23` and
# `-std=c23`, where MSVC offered a `latest` that stood for most of each and
# meant something different every year -- and the ABI header is compiled as C23
# by a C compiler that has C23's enums (tests/abi_header_c.c).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(CompilerOptions)
include(Module)
include(Rust)

# The module ABI. Header only, pure C, and the one thing every layer may see.
# The decode-quality test needs FFmpeg to build its corpus, and skips itself
# without one. That is right locally and wrong in CI, where a check that
# silently does not run is worse than no check at all.
option(MEDIAPERCH_REQUIRE_FFMPEG
       "Fail the decode-quality test when FFmpeg is missing rather than skipping it" OFF)

# **Two of the matrix's columns are not this tree's to promise.** `demux_ffmpeg`
# runs whatever FFmpeg is installed and `demux_mf` asks Media Foundation, whose
# codec set differs between a desktop and a server SKU -- so those two cells are
# what *this machine* does, and a second machine measuring them differently is
# information rather than a regression. Strict compares every column and is
# right where the table is written; CI turns it off and still holds every column
# whose behaviour is code in this repository.
option(MEDIAPERCH_MATRIX_STRICT
       "Compare every column of the format matrix, including the ones that delegate" ON)

add_library(mediaperch_abi INTERFACE)
add_library(MediaPerch::abi ALIAS mediaperch_abi)
target_include_directories(mediaperch_abi INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")

add_subdirectory(src/engine)
add_subdirectory(src/player)

# **Two settings for the submodules' CMake, which is older than ours.**
#
# libogg, libvorbis, libebml and libmatroska declare cmake_minimum_required
# 3.5 or 3.6, and CMake 4 warns on every configure that compatibility with
# anything below 3.10 is going away. The same declaration leaves CMP0069 unset
# in those projects, so the INTERPROCEDURAL_OPTIMIZATION this tree sets for
# Release was being *ignored* for six of their targets -- silently, apart from
# one warning per target. CMAKE_POLICY_VERSION_MINIMUM raises the policy
# version they are given to 3.10, which ends the deprecation warning and makes
# CMP0069 NEW, so the submodules get the LTO everything else already gets.
# Verified on a two-line project before it was trusted here.
#
# CMP0183 (3.31) is libebml's add_feature_info() calls; NEW is the full
# condition syntax, which the ON/OFF values they pass satisfy either way.
#
# Set here rather than in each module's CMakeLists because they are properties
# of *dependencies*, not of any one module, and a default set after a submodule
# was added would miss it. Neither touches this project's own policy version,
# which cmake_minimum_required at the top sets.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(CMAKE_POLICY_DEFAULT_CMP0183 NEW)

# Portable modules build wherever the core does.
if(MEDIAPERCH_BUILD_PLATFORM)
    # **Containers, then codecs, then everything else.** One ordering
    # constraint is real and the rest is readability: libvorbis asks
    # `find_package(Ogg REQUIRED)`, and cmake/FindOgg.cmake answers with the
    # target modules/demux/ogg creates -- so the Ogg container comes before the
    # Vorbis codec. Each submodule now sits with the one module that needs it,
    # which is what made that the only ordering left.

    # codec/flac is where external/flac is brought in, and demux/flac links the
    # same target -- so the codec comes first, and the container reader after it.
    add_subdirectory(modules/codec/flac)

    # codec/mpa is where external/mpg123 is brought in, and demux/mpa links the
    # same target -- so it comes before the demuxers for the same reason.
    add_subdirectory(modules/codec/mpa)

    add_subdirectory(modules/demux/wav)
    add_subdirectory(modules/demux/flac)
    add_subdirectory(modules/demux/mpa)
    add_subdirectory(modules/demux/adts)
    add_subdirectory(modules/demux/dsd)
    add_subdirectory(modules/demux/wavpack)
    add_subdirectory(modules/demux/mp4)
    add_subdirectory(modules/demux/ogg)
    add_subdirectory(modules/demux/mkv)

    add_subdirectory(modules/codec/pcm)
    add_subdirectory(modules/codec/aac)
    add_subdirectory(modules/codec/dsd)
    add_subdirectory(modules/codec/alac)
    add_subdirectory(modules/codec/opus)
    add_subdirectory(modules/codec/vorbis)

    add_subdirectory(modules/shared/abi_guard)
    add_subdirectory(modules/dsp/gain)
    add_subdirectory(modules/dsp/resample)
    add_subdirectory(modules/shared/module_log)
    add_subdirectory(modules/shared/decoder_threads)
    add_subdirectory(modules/shared/dr_wav)
    add_subdirectory(modules/shared/h264)
    add_subdirectory(modules/shared/cube)
    add_subdirectory(modules/shared/kernels)
    add_subdirectory(modules/shared/pcm_format)
    add_subdirectory(modules/shared/transform)
    add_subdirectory(modules/shared/biquad)
    add_subdirectory(modules/shared/convolve)
    add_subdirectory(modules/dsp/mix)
    add_subdirectory(modules/dsp/eq)
    add_subdirectory(modules/dsp/replaygain)
    add_subdirectory(modules/dsp/convolve)
    add_subdirectory(modules/dsp/vst3)
    add_subdirectory(modules/codec/mft)
    add_subdirectory(modules/codec/dav1d)
    add_subdirectory(modules/codec/aom)
    add_subdirectory(modules/codec/avm)
    add_subdirectory(modules/codec/vpx)
    add_subdirectory(modules/codec/de265)
    add_subdirectory(modules/video/d3d11)
    add_subdirectory(modules/vdsp/lut)
    add_subdirectory(modules/vdsp/scale)
    if(MEDIAPERCH_BUILD_ABI_PROBES)
        add_subdirectory(abi/probe_c)
    endif()
endif()

if(MEDIAPERCH_BUILD_PLATFORM AND WIN32)
    # The two pipelines: a file at one end and PCM at the other, with no seam
    # inside for this tree to take hold of. Both are MP_KIND_DEMUX with every
    # stream flagged MP_STREAM_SELF_DECODES, which is that stated rather than
    # disguised.
    add_subdirectory(modules/demux/ffmpeg)
    add_subdirectory(modules/demux/mf)
    add_subdirectory(modules/sink/wasapi)
    add_subdirectory(modules/sink/asio)
    add_subdirectory(src/win)
    add_subdirectory(shell/winui)
endif()

if(MEDIAPERCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MEDIAPERCH_BUILD_FUZZERS)
    enable_testing()
    add_subdirectory(fuzz)
endif()

# Last, so that every target exists: see the function's comment.
mediaperch_flags_everywhere("${CMAKE_SOURCE_DIR}")

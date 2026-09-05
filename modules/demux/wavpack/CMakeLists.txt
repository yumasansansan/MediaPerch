# SPDX-License-Identifier: GPL-3.0-or-later
#
# WavPack, on libwavpack -- and where the submodule enters the build.
#
# One module, not two: libwavpack has no block-level API to split a container
# from a codec with, so this names the codec its payload actually is --
# MP_CODEC_PCM, or MP_CODEC_DSD for a DSD file -- and hands packets to the
# module that already reads it. demux_wavpack.cpp has the argument.
#
# BSD-3-Clause, which a GPL-3.0 program may link. See docs/plan.md for the
# licence table.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/wavpack/CMakeLists.txt")
    message(WARNING
        "external/wavpack is missing -- run `git submodule update --init`. "
        "Skipping demux_wavpack; the tree still builds without it.")
    return()
endif()

# The command line tools are a whole second program, and the CMake package and
# pkg-config files are for an install this never does. Threads stay on: they are
# libwavpack's own worker pool for decoding, off by default at the API and asked
# for per file, so nothing starts a thread this module did not ask it to.
set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_DSD ON CACHE BOOL "" FORCE)

# **The assembly is on, and getting it reliably on took working around a bug
# upstream.**
#
# WavPack's CMakeLists calls `enable_language(ASM_MASM)` guarded by
# `WavPack_CPU_X64` -- and sets that variable ninety lines *later*, in
# `WavPack_DetectTargetCPUArchitectures`. On a first configure the guard is
# therefore false and the assembly is off; the detection then caches the
# variable, so the *second* configure sees it, enables MASM, and turns the
# assembly on. Measured: a fresh tree had `WAVPACK_ENABLE_ASM` empty and a tree
# configured twice had it `ON`, from the same source. That is a build whose
# output depends on how many times it has been configured.
#
# Enabling the language here, before the subdirectory is added, makes the guard
# true the first time. **There is no reproducibility cost to pay for the speed**:
# WavPack is lossless, so the two paths must agree or one is broken, and they do
# -- five files including a lossy hybrid one, identical bytes with and without.
# The gain is 13%: 60 seconds of 24-bit 96 kHz in 0.336 s rather than 0.387 s.
if(MSVC)
    enable_language(ASM_MASM)
endif()
set(WAVPACK_ENABLE_ASM ON CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_LEGACY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/wavpack"
                 "${CMAKE_BINARY_DIR}/external/wavpack" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_demux_wavpack
    KIND demux
    SOURCES demux_wavpack.cpp
    LINK wavpack)

# Not ours, and not held to our warning set.
if(TARGET wavpack)
    # Guarded by language for the reason cmake/CompilerOptions.cmake gives at
    # length: libwavpack has MASM in it, and a warning level is not a thing an
    # assembler has.
    if(MSVC)
        target_compile_options(wavpack PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/W0>")
    else()
        target_compile_options(wavpack PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-w>")
    endif()
    set_target_properties(wavpack PROPERTIES FOLDER "external")
endif()

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_wavpack PRIVATE mediaperch_pcm_format)

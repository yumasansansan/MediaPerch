# SPDX-License-Identifier: GPL-3.0-or-later
#
# MPEG audio layers I to III, on libmpg123 -- and where mpg123 enters the build.
#
# This directory owns the submodule the way modules/codec/flac owns libFLAC:
# both halves of MPEG audio use it, so the codec brings it in and demux/mpa
# links the same target. That is why the root CMakeLists adds this directory
# before modules/demux/mpa.
#
# LGPL 2.1, which a GPL-3.0 program may link. See docs/plan.md for the licence
# table and docs/building.md for why the pin is a mirror commit rather than a
# release tag: mpg123's upstream is SVN and has no git releases at all.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/mpg123/ports/cmake/CMakeLists.txt")
    message(WARNING
        "external/mpg123 is missing -- run `git submodule update --init`. "
        "Skipping codec_mpa; the tree still builds without it.")
    return()
endif()

# **mpg123's optimised decoders are wanted, and they need `yasm`.**
#
# `ports/cmake` looks for it on PATH; without it `MACHINE` silently falls back
# to `generic` and libmpg123 is built with none of OPT_MULTI, OPT_X86_64 or
# OPT_AVX. It is a fork in the build that was taken by accident on both machines
# this has run on -- `yasm.exe` ships inside Strawberry Perl, which is on PATH
# here and pre-installed on GitHub's Windows runners -- and nobody asked for it.
#
# **The two decoders do not produce the same bytes.** Measured on the same
# 2-second MP3: `69dca145…` with the AVX synthesis and `f6c8a8e4…` with the
# generic one, agreeing to 127.84 dB with a maximum difference of 1.5e-7 and
# 10% of samples identical. That is float rounding between two implementations
# of one synthesis filter, and both are correct by the RMS bound ISO 11172-4
# calls conformance -- but only one of them is the hash docs/formats.md records.
#
# So the fork is *named* rather than left silent. The optimised path is the one
# this tree wants: 60 seconds of 320 kbps MP3 in 0.179 s against 0.196 s, and
# an engine that spends less time decoding has more left for the render thread.
# What a build without yasm gets is a working decoder whose bytes are not the
# recorded ones, and it now says so instead of being found out by a hash.
find_program(MEDIAPERCH_YASM yasm)
if(NOT MEDIAPERCH_YASM)
    message(WARNING
        "yasm is not on PATH, so libmpg123 falls back to its generic decoders. "
        "MP3 still plays and is still correct -- but about 9% slower, and its "
        "bytes differ from the hash docs/formats.md records by 128 dB of float "
        "rounding. Install yasm (it ships with Strawberry Perl) for the build "
        "this project measures.")
endif()

# libout123 is mpg123's *output* layer -- ALSA, WASAPI, a WAV writer. This tree
# has its own sink and wants none of it, and turning it off takes the command
# line programs with it.
set(BUILD_LIBOUT123 OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/mpg123/ports/cmake"
                 "${CMAKE_BINARY_DIR}/external/mpg123" EXCLUDE_FROM_ALL SYSTEM)

# **mpg123's CMake sets its include paths with directory-scoped
# `include_directories`, not on the target**, so `libmpg123` carries no usage
# requirements and a consumer gets `fatal error C1083: mpg123.h`. The headers
# ship in the source tree -- they are not generated -- so one INTERFACE path on
# the target says so once, here, and both modules inherit it by linking.
if(TARGET libmpg123)
    target_include_directories(libmpg123
        INTERFACE "${CMAKE_SOURCE_DIR}/external/mpg123/src/include")
endif()

mediaperch_add_module(mp_codec_mpa
    KIND codec
    SOURCES codec_mpa.cpp
    LINK libmpg123)

# Not ours, and not held to our warning set.
foreach(_ext IN ITEMS libmpg123 libsyn123 compat compat_str)
    if(TARGET ${_ext})
        if(MSVC)
            target_compile_options(${_ext} PRIVATE /W0)
        else()
            target_compile_options(${_ext} PRIVATE -w)
        endif()
        set_target_properties(${_ext} PROPERTIES FOLDER "external")
    endif()
endforeach()

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_codec_mpa PRIVATE mediaperch_module_log)

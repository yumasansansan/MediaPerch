# SPDX-License-Identifier: GPL-3.0-or-later
#
# Opus on libopus itself rather than through opusfile, which is the container
# and the codec in one. This module has never heard of Ogg.
#
# opusfile is not in the tree at all any more. It went with `decode_ogg`, and
# with it went its HTTP support -- a decoder that can open a socket is a decoder
# with an attack surface it did not need, and turning that off was a setting
# somebody had to remember rather than a thing that was absent.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/opus/CMakeLists.txt")
    message(WARNING
        "external/opus is empty -- run `git submodule update --init`. "
        "Skipping codec_opus; the tree still builds without it.")
    return()
endif()

# The version libopus reports, pinned here beside the gitlink that decides it.
# cmake/OpusPackageVersion.cmake reads this instead of running `git describe`,
# which upstream does and which fails in any checkout without tags.
set(MEDIAPERCH_OPUS_VERSION "1.6.1")      # external/opus at v1.6.1

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# **In the AVX2 build, opus is told to stop checking.**
#
# libopus compiles an SSE, an SSE2, an SSE4.1 and an AVX2 path and picks one per
# call site by asking the CPU. In a binary that already refuses to start without
# AVX2 that question has one answer, and asking it costs a branch on a hot path
# and keeps three code paths alive that can never run. `PRESUME` is opus's own
# word for "stop asking": the dispatch goes, and so do the paths it chose
# between.
#
# The two below `SSE4_1` are already presumed by opus's own defaults on x86-64;
# they are named anyway so that all four read as one decision rather than two
# defaults and two overrides.
#
# libFLAC and libmpg123 dispatch internally too and offer nothing to turn it off
# with -- FLAC's is not an option and mpg123's `OPT_MULTI` is a local `set()` in
# its own list file. Their checks stay, and are a branch each.
if(MEDIAPERCH_ARCH STREQUAL "avx2")
    set(OPUS_X86_PRESUME_SSE ON CACHE BOOL "" FORCE)
    set(OPUS_X86_PRESUME_SSE2 ON CACHE BOOL "" FORCE)
    set(OPUS_X86_PRESUME_SSE4_1 ON CACHE BOOL "" FORCE)
    set(OPUS_X86_PRESUME_AVX2 ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/external/opus"
                 "${CMAKE_BINARY_DIR}/external/opus" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_codec_opus
    KIND codec
    SOURCES codec_opus.cpp
    LINK Opus::opus)

if(TARGET opus)
    if(MSVC)
        target_compile_options(opus PRIVATE /W0)
    else()
        target_compile_options(opus PRIVATE -w)
    endif()
    set_target_properties(opus PROPERTIES FOLDER "external")
endif()

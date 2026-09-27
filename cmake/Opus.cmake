# SPDX-License-Identifier: GPL-3.0-or-later
#
# libopus, told the instruction set, as one build of it: included by
# modules/codec/opus, which decodes with it, and by the fuzz tree, which fuzzes
# it -- so that the fuzzers run the paths the player runs. The fuzz tree turns
# OPUS_ASSERTIONS on before it includes this.
#
# The version libopus reports is pinned in cmake/OpusPackageVersion.cmake, which
# upstream's list file reaches instead of running `git describe` -- which fails
# in any checkout without tags.
#
# **Every build tells opus to stop checking.**
#
# libopus compiles an SSE, an SSE2, an SSE4.1 and an AVX2 path and picks one per
# call site by asking the CPU. In a binary that cannot run without AVX2 at all
# -- which every build here is -- that question has one answer, and asking
# it costs a branch on a hot path and keeps three code paths alive that can
# never run. `PRESUME` is opus's own word for "stop asking": the dispatch goes,
# and so do the paths it chose between.
#
# The two below `SSE4_1` are already presumed by opus's own defaults on x86-64;
# they are named anyway so that all four read as one decision rather than two
# defaults and two overrides.
#
# The AVX-512 build presumes the same four, because AVX2 is where opus's list
# ends: it has no AVX-512 path to presume. What AVX-512 code it has (dnn/) is
# chosen at compile time by the instruction set itself, so `-march=x86-64-v4`
# is all it needs.
#
# libFLAC and libmpg123 dispatch internally too and offer nothing to turn it off
# with -- FLAC's is not an option and mpg123's `OPT_MULTI` is a local `set()` in
# its own list file. Their checks stay, and are a branch each.

if(TARGET opus OR NOT EXISTS "${CMAKE_SOURCE_DIR}/external/opus/CMakeLists.txt")
    return()
endif()

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_X86_PRESUME_SSE ON CACHE BOOL "" FORCE)
set(OPUS_X86_PRESUME_SSE2 ON CACHE BOOL "" FORCE)
set(OPUS_X86_PRESUME_SSE4_1 ON CACHE BOOL "" FORCE)
set(OPUS_X86_PRESUME_AVX2 ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/opus"
                 "${CMAKE_BINARY_DIR}/external/opus" EXCLUDE_FROM_ALL SYSTEM)

# Not ours, and not held to our warning set.
target_compile_options(opus PRIVATE -w)
set_target_properties(opus PROPERTIES FOLDER "external")

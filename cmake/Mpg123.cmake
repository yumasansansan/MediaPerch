# SPDX-License-Identifier: GPL-3.0-or-later
#
# libmpg123, with its optimised decoders, as one build of it: included by
# modules/codec/mpa, which brings it in for both halves of MPEG audio, and by
# the fuzz tree, which fuzzes it -- so that the fuzzers run the decoders the
# player runs.
#
# **The optimised decoders are the ones this tree ships, on every system.**
# They are libmpg123's x86-64 assembly -- `.S` files, which CMake compiles as
# ASM, with Clang -- chosen by `ports/cmake` whenever the machine is x86-64. The
# one way it had been lost was MSVC's: `ports/cmake` asks for yasm when the
# compiler is MSVC itself and falls back to the generic decoders without it,
# which happened by accident on both machines this ran on while MSVC built the
# tree. Clang takes no such branch; it is checked below all the same, so that
# no build on any system gets the generic decoders without failing to configure.
#
# **The two decoders do not produce the same bytes.** Measured on the same
# 2-second MP3: `69dca145…` with the AVX synthesis and `f6c8a8e4…` with the
# generic one, agreeing to 127.84 dB with a maximum difference of 1.5e-7 and
# 10% of samples identical. That is float rounding between two implementations
# of one synthesis filter, and both are correct by the RMS bound ISO 11172-4
# calls conformance -- but only one of them is the hash docs/formats.md records,
# and the optimised one is faster: 60 seconds of 320 kbps MP3 in 0.179 s against
# 0.196 s.
#
# libout123 is mpg123's *output* layer -- ALSA, WASAPI, a WAV writer. This tree
# has its own sink and wants none of it, and turning it off takes the command
# line programs with it.

if(TARGET libmpg123 OR NOT EXISTS "${CMAKE_SOURCE_DIR}/external/mpg123/ports/cmake/CMakeLists.txt")
    return()
endif()

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
# the target says so once, here, and everything that links it inherits it.
target_include_directories(libmpg123
    INTERFACE "${CMAKE_SOURCE_DIR}/external/mpg123/src/include")

# Not ours, and not held to our warning set.
foreach(mediaperch_mpg123_target IN ITEMS libmpg123 libsyn123 compat compat_str)
    if(TARGET ${mediaperch_mpg123_target})
        target_compile_options(${mediaperch_mpg123_target} PRIVATE -w)
        set_target_properties(${mediaperch_mpg123_target} PROPERTIES FOLDER "external")
    endif()
endforeach()

# The optimised decoders, or no build: `ports/cmake` names them in the
# definitions it gives the library.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
    get_target_property(mediaperch_mpg123_definitions libmpg123 COMPILE_DEFINITIONS)
    if(NOT "OPT_X86_64" IN_LIST mediaperch_mpg123_definitions
       OR NOT "OPT_AVX" IN_LIST mediaperch_mpg123_definitions)
        message(FATAL_ERROR
            "libmpg123 was configured without its x86-64 decoders (its definitions "
            "are: ${mediaperch_mpg123_definitions}). They are the decoders this tree "
            "ships and the ones docs/formats.md records the bytes of, so a build without "
            "them stops here rather than decoding MP3 another way.")
    endif()
endif()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# A second front end over every portable source, and nothing else.
#
# **The modules are only ever compiled by MSVC, and that is a hole.** The two
# Clang presets in this tree -- `fuzz` and `asan` -- both set
# MEDIAPERCH_BUILD_PLATFORM=OFF, because the Windows head does not build with a
# GNU-driver Clang. So the modules, which are most of the code, see one compiler,
# and a whole class of diagnostic is invisible.
#
# It cost something already. `MP_CODEC_INTERNAL` was written as an enumerator of
# 0xFFFFFFFF; MSVC types an unscoped enum as `int` regardless of its values and
# said nothing, and the constant was quietly -1. Clang caught it the first time a
# file that used it happened to be compiled into a fuzzer. Three modules were
# keeping a host pointer they never read, and two CRC loops mixed signed and
# unsigned in a tree that compiles with -Wconversion on purpose. All of it was
# invisible for the same reason.
#
# Syntax-only, so there is nothing to link and no Windows head to worry about:
# `clang++ -fsyntax-only` parses and type-checks and stops. It runs in about a
# second for the whole tree and needs no build directory of its own.
#
# Skipped, loudly, when there is no clang and when the include paths a source
# needs are not there -- a check that silently does not run is worse than no
# check, so it says which files it looked at.

if(NOT DEFINED MEDIAPERCH_ROOT)
    message(FATAL_ERROR "ClangSyntax.cmake needs MEDIAPERCH_ROOT")
endif()

find_program(clang_exe NAMES clang++ clang++.exe)
if(NOT clang_exe)
    message(STATUS "no clang++ on PATH -- skipping the second front end")
    return()
endif()

set(includes
    "-I${MEDIAPERCH_ROOT}/include"
    "-I${MEDIAPERCH_ROOT}/src/core"
    "-I${MEDIAPERCH_ROOT}/modules/shared/transform"
    "-I${MEDIAPERCH_ROOT}/modules/shared/biquad"
    "-I${MEDIAPERCH_ROOT}/modules/shared/convolve"
    "-I${MEDIAPERCH_ROOT}/modules/codec/aac"
    "-I${MEDIAPERCH_ROOT}/external/dragonperch/src/core")

# **Somebody else's headers go in with `-isystem`**, which is what stops their
# diagnostics from being ours. libebml and libmatroska are full of conversions
# this tree would not write and is not going to fix; what matters is the
# diagnostics in the file being checked.
set(system_includes
    "-isystem" "${MEDIAPERCH_ROOT}/external/dr_libs"
    "-isystem" "${MEDIAPERCH_ROOT}/external/flac/include"
    "-isystem" "${MEDIAPERCH_ROOT}/external/libebml"
    "-isystem" "${MEDIAPERCH_ROOT}/external/libmatroska"
    "-isystem" "${MEDIAPERCH_ROOT}/external/ogg/include"
    "-isystem" "${MEDIAPERCH_ROOT}/external/Bento4/Source/C++/Core"
    "-isystem" "${MEDIAPERCH_ROOT}/external/Bento4/Source/C++/Codecs"
    "-isystem" "${MEDIAPERCH_ROOT}/external/Bento4/Source/C++/Crypto"
    "-isystem" "${MEDIAPERCH_ROOT}/external/Bento4/Source/C++/MetaData")

# libebml and libmatroska generate an export header into the build tree. Without
# a build directory to point at, the sources that need them are skipped -- and
# the skip is reported, because a check that silently does not run is worse than
# no check.
if(MEDIAPERCH_BUILD_DIR)
    list(APPEND system_includes
        "-isystem" "${MEDIAPERCH_BUILD_DIR}/external/libebml"
        "-isystem" "${MEDIAPERCH_BUILD_DIR}/external/libmatroska"
        "-isystem" "${MEDIAPERCH_BUILD_DIR}/external/ogg/include")
endif()

# The warning set the tree builds with, minus the one a pure-C header cannot
# satisfy: MP_MAKE_VERSION is a macro with C casts in it, and rewriting the ABI
# header in C++ to please a C++ warning would be the tail wagging the dog.
set(warnings
    -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor
    -Wno-old-style-cast)

# Everything that does not need the Windows SDK or a submodule's headers. The
# platform head, the sink and the two pipelines are left out on purpose: they are
# Windows-only by definition, and MSVC is the compiler they are written for.
set(sources
    modules/shared/transform/transform.cpp
    modules/shared/biquad/biquad.cpp
    modules/shared/convolve/convolve.cpp
    modules/codec/pcm/codec_pcm.cpp
    modules/codec/aac/aac.cpp
    modules/codec/aac/aac_tables.cpp
    modules/codec/aac/codec_aac.cpp
    modules/demux/wav/demux_wav.cpp
    modules/demux/flac/demux_flac.cpp
    modules/demux/mpeg/demux_mpeg.cpp
    modules/demux/adts/demux_adts.cpp
    modules/demux/mp4/demux_mp4.cpp)

# These need a header the build generates, so they are checked only when a build
# directory is named.
set(generated_sources
    modules/demux/ogg/demux_ogg.cpp
    modules/demux/mkv/demux_mkv.cpp)
if(MEDIAPERCH_BUILD_DIR AND EXISTS "${MEDIAPERCH_BUILD_DIR}/external/libebml/ebml_export.h")
    list(APPEND sources ${generated_sources})
else()
    foreach(source IN LISTS generated_sources)
        list(APPEND skipped "${source} (needs a configured build directory)")
    endforeach()
endif()

set(checked 0)
set(failed "")
foreach(source IN LISTS sources)
    set(full "${MEDIAPERCH_ROOT}/${source}")
    if(NOT EXISTS "${full}")
        list(APPEND skipped "${source} (missing)")
        continue()
    endif()
    execute_process(
        COMMAND "${clang_exe}" -fsyntax-only -std=c++23
                -target x86_64-pc-windows-msvc ${warnings} ${includes}
                ${system_includes} "${full}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said ERROR_VARIABLE complained)
    if(NOT status EQUAL 0 OR complained MATCHES "warning:")
        list(APPEND failed "${source}")
        message("---- ${source}")
        message("${complained}")
    endif()
    math(EXPR checked "${checked} + 1")
endforeach()

message(STATUS "clang parsed ${checked} sources")
if(skipped)
    message(STATUS "skipped: ${skipped}")
endif()
if(failed)
    list(LENGTH failed how_many)
    message(FATAL_ERROR
        "${how_many} source(s) that MSVC accepts have a diagnostic from clang: ${failed}")
endif()

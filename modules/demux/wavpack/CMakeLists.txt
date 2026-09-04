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
    if(MSVC)
        target_compile_options(wavpack PRIVATE /W0)
    else()
        target_compile_options(wavpack PRIVATE -w)
    endif()
    set_target_properties(wavpack PROPERTIES FOLDER "external")
endif()

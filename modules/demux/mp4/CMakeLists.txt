# SPDX-License-Identifier: GPL-3.0-or-later
#
# MP4 as a container and nothing else, on Bento4 from Axiomatic Systems.
#
# It was a parser written here until it wasn't. The reason for the change is the
# reason recorded for FLAC and Matroska: where a reference implementation of the
# container exists, this tree reads the container with it. The five hundred lines
# that went away read twelve boxes of ISO 14496-12; `ap4` reads the standard,
# which is what brings fragmented MP4, `.mov` and multi-track files with it.
#
# GPL-2.0-**or-later**, which matters: GPLv2-only would not be linkable from a
# GPL-3.0-or-later program, and "or, at your option, any later version" in every
# file header is what makes it so. See docs/plan.md for the licence table.
#
# Pinned past its last release tag on purpose. Bento4 tags rarely -- 119 commits
# separate v1.6.0-641 from this one -- and among them are a heap overflow in
# AP4_BitReader::ReadCache and a leak in AP4_LinearReader::Tracker, which is a
# class this module uses on every fragmented file.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/Bento4/CMakeLists.txt")
    message(WARNING
        "external/Bento4 is missing -- run `git submodule update --init`. "
        "Skipping demux_mp4; the tree still builds without it.")
    return()
endif()

# Three of its defaults are wrong for this tree, and the third is not cosmetic:
# `ap4` would otherwise be compiled against the *static* CRT while every module
# here uses the dynamic one, which is two heaps in one process.
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MSVC_FORCE_STATIC_C_RUNTIME OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/Bento4"
                 "${CMAKE_BINARY_DIR}/external/Bento4" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_demux_mp4
    KIND demux
    SOURCES demux_mp4.cpp
    LINK ap4)

# Not ours, and not held to our warning set.
if(TARGET ap4)
    if(MSVC)
        target_compile_options(ap4 PRIVATE /W0)
    else()
        target_compile_options(ap4 PRIVATE -w)
    endif()
    set_target_properties(ap4 PROPERTIES FOLDER "external")
endif()

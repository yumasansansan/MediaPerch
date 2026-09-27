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

# The library, with its assembly, as the fuzz tree builds it too:
# cmake/WavPack.cmake, which also stops a build that would lose it.
include(WavPack)

mediaperch_add_module(mp_demux_wavpack
    KIND demux
    SOURCES demux_wavpack.cpp
    LINK wavpack)

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_wavpack PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_wavpack PRIVATE mediaperch_module_log)

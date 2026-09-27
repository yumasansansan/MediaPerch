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

# The library, with its optimised decoders, as the fuzz tree builds it too:
# cmake/Mpg123.cmake, which also stops a build that would lose them.
include(Mpg123)

mediaperch_add_module(mp_codec_mpa
    KIND codec
    SOURCES codec_mpa.cpp
    LINK libmpg123)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_codec_mpa PRIVATE mediaperch_module_log)

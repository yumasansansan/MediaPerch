# SPDX-License-Identifier: GPL-3.0-or-later
#
# **The audio engine, and nothing that decides what to play.**
#
# It knows about formats, negotiation, the ring, the two graphs, the conversion
# and the DSP chain. It does not know about a transport, a playlist, a settings
# file or a wire protocol -- those are `src/player`, and this directory has no
# route to them: `src/player` is not on its include path, so a `#include
# "mediaperch/player.hpp"` from here is a compile error rather than a review
# comment.
#
# That cut is why this is a separate library rather than a folder. §4 of the
# plan asks whether this ABI could carry a DAW's engine and answers yes; a DAW
# taking it would want everything here and none of `src/player`, because a
# player's transport is a file position and a DAW's is a musical one. Until
# this split, taking one meant taking both.
#
# Portable: no OS headers, no platform conditionals, checked by
# cmake/CorePurity.cmake as a test rather than as a guideline.

add_library(mediaperch_engine STATIC
    mediaperch/avsync.cpp
    mediaperch/avsync.hpp
    mediaperch/compare.cpp
    mediaperch/compare.hpp
    mediaperch/convert.cpp
    mediaperch/convert.hpp
    mediaperch/dither.cpp
    mediaperch/dither.hpp
    mediaperch/dsp.cpp
    mediaperch/dsp.hpp
    mediaperch/shaper_tables.cpp
    mediaperch/shaper_tables.hpp
    mediaperch/format.cpp
    mediaperch/format.hpp
    mediaperch/negotiation.cpp
    mediaperch/negotiation.hpp
    mediaperch/log.cpp
    mediaperch/log.hpp
    mediaperch/packet.cpp
    mediaperch/packet.hpp
    mediaperch/passthrough.cpp
    mediaperch/passthrough.hpp
    mediaperch/processed.cpp
    mediaperch/processed.hpp
    mediaperch/processor.cpp
    mediaperch/processor.hpp
    mediaperch/video.cpp
    mediaperch/video.hpp
    mediaperch/display.cpp
    mediaperch/display.hpp
    mediaperch/repack.cpp
    mediaperch/repack.hpp
    mediaperch/result.cpp
    mediaperch/result.hpp
    mediaperch/ring.hpp
    mediaperch/sine.cpp
    mediaperch/sine.hpp
    mediaperch/sink.cpp
    mediaperch/sink.hpp
    mediaperch/source.hpp
)
add_library(MediaPerch::engine ALIAS mediaperch_engine)

target_include_directories(mediaperch_engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_engine PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)

find_package(Threads REQUIRED)
target_link_libraries(mediaperch_engine PUBLIC Threads::Threads)

set_target_properties(mediaperch_engine PROPERTIES FOLDER "core")

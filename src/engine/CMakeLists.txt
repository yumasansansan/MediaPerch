# SPDX-License-Identifier: GPL-3.0-or-later
#
# **The two engines, and nothing that decides what to play.**
#
# The audio engine knows about formats, negotiation, the ring, the two graphs,
# the conversion and the DSP chain; the video engine about decoders, presenters,
# the display loop and the clock a picture follows. Neither knows about a
# transport, a playlist, a settings file or a wire protocol -- those are
# `src/player`, and this directory has no route to them: `src/player` is not on
# its include path, so a `#include "mediaperch/player.hpp"` from here is a
# compile error rather than a review comment.
#
# That cut is why this is a separate library rather than a folder. §4 of the
# plan asks whether this ABI could carry a DAW's engine and answers yes; a DAW
# taking it would want everything here and none of `src/player`, because a
# player's transport is a file position and a DAW's is a musical one. Until
# this split, taking one meant taking both.
#
# Portable: no OS headers, no platform conditionals, checked by
# cmake/CorePurity.cmake as a test rather than as a guideline.

# **Three libraries, and the rule is a link line.** The audio engine and the
# video engine are built without each other: `mediaperch_audio` never includes
# a video header and `mediaperch_video` never includes an audio one, and both
# stand on `mediaperch_core` -- formats, the module loaders, the clock the two
# hand each other, and the interfaces a file is opened into. A product that
# wants the picture and not the sound links `MediaPerch::video` and brings its
# own `IVideoHost`; one that wants the sound links `MediaPerch::audio`.
# tests/audio_alone.cpp and tests/video_alone.cpp do exactly that, and
# cmake/CorePurity.cmake reads the includes so a header cannot cross over
# without failing a test. `MediaPerch::engine` is the pair, for the player.
find_package(Threads REQUIRED)

add_library(mediaperch_core STATIC
    mediaperch/clock.cpp
    mediaperch/clock.hpp
    mediaperch/format.cpp
    mediaperch/format.hpp
    mediaperch/log.cpp
    mediaperch/log.hpp
    mediaperch/packet.cpp
    mediaperch/packet.hpp
    mediaperch/rational.hpp
    mediaperch/result.cpp
    mediaperch/result.hpp
    mediaperch/source.hpp
)
add_library(MediaPerch::core ALIAS mediaperch_core)
target_include_directories(mediaperch_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_core
    PUBLIC MediaPerch::abi Threads::Threads
    PRIVATE mediaperch_flags)
set_target_properties(mediaperch_core PROPERTIES FOLDER "core")

add_library(mediaperch_audio STATIC
    mediaperch/buffering.cpp
    mediaperch/buffering.hpp
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
    mediaperch/negotiation.cpp
    mediaperch/negotiation.hpp
    mediaperch/passthrough.cpp
    mediaperch/passthrough.hpp
    mediaperch/processed.cpp
    mediaperch/processed.hpp
    mediaperch/processor.cpp
    mediaperch/processor.hpp
    mediaperch/repack.cpp
    mediaperch/repack.hpp
    mediaperch/ring.hpp
    mediaperch/sine.cpp
    mediaperch/sine.hpp
    mediaperch/sink.cpp
    mediaperch/sink.hpp
    mediaperch/wiring.cpp
    mediaperch/wiring.hpp
)
add_library(MediaPerch::audio ALIAS mediaperch_audio)
target_link_libraries(mediaperch_audio PUBLIC MediaPerch::core PRIVATE mediaperch_flags)
set_target_properties(mediaperch_audio PROPERTIES FOLDER "core")

add_library(mediaperch_video STATIC
    mediaperch/avsync.cpp
    mediaperch/avsync.hpp
    mediaperch/display.cpp
    mediaperch/display.hpp
    mediaperch/framerate.cpp
    mediaperch/framerate.hpp
    mediaperch/refresh.cpp
    mediaperch/refresh.hpp
    mediaperch/video.cpp
    mediaperch/video.hpp
    mediaperch/video_host.hpp
    mediaperch/video_path.cpp
    mediaperch/video_path.hpp
)
add_library(MediaPerch::video ALIAS mediaperch_video)
target_link_libraries(mediaperch_video PUBLIC MediaPerch::core PRIVATE mediaperch_flags)
set_target_properties(mediaperch_video PROPERTIES FOLDER "core")

# The pair, for whoever wants both. The player does; a DAW or a viewer would
# take one of the two above instead.
add_library(mediaperch_engine INTERFACE)
add_library(MediaPerch::engine ALIAS mediaperch_engine)
target_link_libraries(mediaperch_engine
    INTERFACE MediaPerch::core MediaPerch::audio MediaPerch::video)

# SPDX-License-Identifier: GPL-3.0-or-later
#
# **What decides what to play**, on top of the engine that plays it.
#
# Four things, and each one is the thing another program would bring its own of:
# `player` is the transport and the state a shell subscribes to, `queue` is the
# playlist and the gapless join between tracks, `settings` is the INI schema,
# and `protocol` is the wire format a shell speaks. A DAW's transport is a
# musical position rather than a file position, its project file is not an INI,
# and its UI is in-process -- so it takes `MediaPerch::engine` and none of this.
#
# The dependency runs one way and is enforced by the include path: this library
# can see `src/engine`, and `src/engine` cannot see this one.
#
# Portable, on the same terms as the engine: no OS headers, no platform
# conditionals, checked by cmake/CorePurity.cmake.

add_library(mediaperch_player STATIC
    mediaperch/player.cpp
    mediaperch/player.hpp
    mediaperch/protocol.cpp
    mediaperch/protocol.hpp
    mediaperch/queue.cpp
    mediaperch/queue.hpp
    mediaperch/settings.cpp
    mediaperch/settings.hpp
    # DragonPerch's INI parser, as a submodule rather than a second copy. The
    # settings file is read by more than one program in this house and a key
    # that meant different things in each would be worse than sharing a file.
    "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core/dragonperch/ini.cpp"
)
add_library(MediaPerch::player ALIAS mediaperch_player)

target_include_directories(mediaperch_player
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core")
target_link_libraries(mediaperch_player
    PUBLIC MediaPerch::engine
    PRIVATE mediaperch_flags)

set_target_properties(mediaperch_player PROPERTIES FOLDER "core")

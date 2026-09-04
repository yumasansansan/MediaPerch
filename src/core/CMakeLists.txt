# SPDX-License-Identifier: GPL-3.0-or-later
#
# The portable half. It knows about formats, negotiation, the ring and the graph,
# and nothing about windows, devices, files or sockets.
#
# Its include path is deliberately just `include/` and this directory: there is
# no route from here to src/win even by accident, and CorePurity.cmake fails the
# build if an OS header appears anyway.

add_library(mediaperch_core STATIC
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
    mediaperch/player.cpp
    mediaperch/player.hpp
    mediaperch/protocol.cpp
    mediaperch/protocol.hpp
    mediaperch/queue.cpp
    mediaperch/queue.hpp
    mediaperch/result.cpp
    mediaperch/result.hpp
    mediaperch/settings.cpp
    mediaperch/settings.hpp
    # DragonPerch's INI parser, as a submodule rather than a second copy. The
    # settings file is read by more than one program in this house and a key
    # that meant different things in each would be worse than sharing a file.
    "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core/dragonperch/ini.cpp"
    mediaperch/processed.cpp
    mediaperch/processor.cpp
    mediaperch/processed.hpp
    mediaperch/repack.cpp
    mediaperch/repack.hpp
    mediaperch/ring.hpp
    mediaperch/sine.cpp
    mediaperch/sine.hpp
    mediaperch/sink.cpp
    mediaperch/sink.hpp
    mediaperch/source.hpp
)
add_library(MediaPerch::core ALIAS mediaperch_core)

target_include_directories(mediaperch_core
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core")
target_link_libraries(mediaperch_core PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)

find_package(Threads REQUIRED)
target_link_libraries(mediaperch_core PUBLIC Threads::Threads)

set_target_properties(mediaperch_core PROPERTIES FOLDER "core")

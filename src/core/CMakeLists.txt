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
    mediaperch/shaper_tables.cpp
    mediaperch/shaper_tables.hpp
    mediaperch/decoder.cpp
    mediaperch/decoder.hpp
    mediaperch/format.cpp
    mediaperch/format.hpp
    mediaperch/negotiation.cpp
    mediaperch/negotiation.hpp
    mediaperch/passthrough.cpp
    mediaperch/passthrough.hpp
    mediaperch/processed.cpp
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

target_include_directories(mediaperch_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_core PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)

find_package(Threads REQUIRED)
target_link_libraries(mediaperch_core PUBLIC Threads::Threads)

set_target_properties(mediaperch_core PROPERTIES FOLDER "core")

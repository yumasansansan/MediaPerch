# SPDX-License-Identifier: GPL-3.0-or-later
#
# The portable half. It knows about formats, negotiation, the ring and the graph,
# and nothing about windows, devices, files or sockets.
#
# Its include path is deliberately just `include/` and this directory: there is
# no route from here to src/win even by accident, and CorePurity.cmake fails the
# build if an OS header appears anyway.

add_library(mediaperch_core STATIC
    mediaperch/format.cpp
    mediaperch/format.hpp
    mediaperch/negotiation.cpp
    mediaperch/negotiation.hpp
    mediaperch/ring.hpp
)
add_library(MediaPerch::core ALIAS mediaperch_core)

target_include_directories(mediaperch_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_core PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)

set_target_properties(mediaperch_core PROPERTIES FOLDER "core")

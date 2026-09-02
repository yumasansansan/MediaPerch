# SPDX-License-Identifier: GPL-3.0-or-later
#
# The only decoder here with no dependency of any kind: no submodule, no runtime
# library, no OS codec. See alac.hpp for why that was worth writing rather than
# linking the reference implementation.
#
# alac.cpp and mp4.cpp are also compiled into a small static library so that the
# fuzzers can drive the parsers without loading a module.

add_library(mediaperch_alac STATIC alac.cpp)
target_include_directories(mediaperch_alac PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_alac PUBLIC mediaperch_mp4 PRIVATE mediaperch_flags)
set_target_properties(mediaperch_alac PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_decode_alac
    KIND decode
    SOURCES decode_alac.cpp
    LINK mediaperch_alac)


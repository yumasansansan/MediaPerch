# SPDX-License-Identifier: GPL-3.0-or-later
#
# FLAC's framing, on its own so that the fuzz target can link it directly. A
# parser reachable only through a module is a parser that cannot be fuzzed.

add_library(mediaperch_flacframe STATIC flacframe.cpp)
target_include_directories(mediaperch_flacframe PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_flacframe PRIVATE mediaperch_flags)
set_target_properties(mediaperch_flacframe PROPERTIES FOLDER "modules")

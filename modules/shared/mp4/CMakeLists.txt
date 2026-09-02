# SPDX-License-Identifier: GPL-3.0-or-later
#
# The slice of MP4 that decode_alac and decode_aac both need. It lived inside
# decode_alac until there were two callers.

add_library(mediaperch_mp4 STATIC mp4.cpp)
target_include_directories(mediaperch_mp4 PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_mp4 PRIVATE mediaperch_flags)
set_target_properties(mediaperch_mp4 PROPERTIES FOLDER "modules")

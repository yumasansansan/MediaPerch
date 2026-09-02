# SPDX-License-Identifier: GPL-3.0-or-later
#
# The slice of MP4 that demux_mp4 needs, and that the ALAC fuzzer links
# directly. It lived inside decode_alac until there were two callers, then
# became a library, then became the container half of a demuxer.

add_library(mediaperch_mp4 STATIC mp4.cpp)
target_include_directories(mediaperch_mp4 PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_mp4 PRIVATE mediaperch_flags)
set_target_properties(mediaperch_mp4 PROPERTIES FOLDER "modules")

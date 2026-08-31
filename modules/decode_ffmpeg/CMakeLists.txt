# SPDX-License-Identifier: GPL-3.0-or-later
#
# FFmpeg is not vendored, not submoduled and not linked: this module finds
# ffmpeg and ffprobe at run time and declines every file when they are absent.
# So there is nothing here to build against -- which is the point.

add_library(mp_decode_ffmpeg MODULE decode_ffmpeg.cpp)

target_link_libraries(mp_decode_ffmpeg PRIVATE MediaPerch::abi mediaperch_flags)

set_target_properties(mp_decode_ffmpeg PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_ffmpeg"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_ffmpeg PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

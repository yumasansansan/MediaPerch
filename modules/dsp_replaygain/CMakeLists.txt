# SPDX-License-Identifier: GPL-3.0-or-later
#
# ReplayGain, measured rather than read. The meter is a library because the
# probe's `loudness` command is the same meter run over a whole file, and
# because a loudness number nobody can reproduce is a loudness number nobody
# should trust.

add_library(mediaperch_loudness STATIC loudness.cpp)
target_include_directories(mediaperch_loudness PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_loudness
    PUBLIC MediaPerch::abi mediaperch_biquad PRIVATE mediaperch_flags)
set_target_properties(mediaperch_loudness PROPERTIES FOLDER "modules")

add_library(mp_dsp_replaygain MODULE dsp_replaygain.cpp)
target_link_libraries(mp_dsp_replaygain
    PRIVATE MediaPerch::abi mediaperch_flags mediaperch_loudness)

set_target_properties(mp_dsp_replaygain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_dsp_replaygain"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_dsp_replaygain PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

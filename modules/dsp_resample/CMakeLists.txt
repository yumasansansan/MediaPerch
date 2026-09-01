# SPDX-License-Identifier: GPL-3.0-or-later
#
# The stage MpDspVtbl was shaped for: one that answers `configure` with a
# different sample rate.
#
# The filter is a library as well as a module so the tests can measure its
# response without loading anything -- the same arrangement as decode_aac, and
# for the same reason: a filter that is only ever tested through a device is a
# filter nobody can put a number on.

add_library(mediaperch_resample STATIC resample.cpp design.cpp)
target_include_directories(mediaperch_resample PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_resample
    PUBLIC mediaperch_transform PRIVATE mediaperch_flags)
set_target_properties(mediaperch_resample PROPERTIES FOLDER "modules")

add_library(mp_dsp_resample MODULE dsp_resample.cpp)
target_link_libraries(mp_dsp_resample
    PRIVATE MediaPerch::abi mediaperch_flags mediaperch_resample)

set_target_properties(mp_dsp_resample PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_dsp_resample"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_dsp_resample PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

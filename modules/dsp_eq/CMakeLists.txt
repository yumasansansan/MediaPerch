# SPDX-License-Identifier: GPL-3.0-or-later
#
# The equaliser. The sections themselves live in modules/biquad, because the
# loudness meter wants the same arithmetic for a cascade nobody chose.

add_library(mp_dsp_eq MODULE dsp_eq.cpp)
target_link_libraries(mp_dsp_eq
    PRIVATE MediaPerch::abi mediaperch_flags mediaperch_biquad)

set_target_properties(mp_dsp_eq PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_dsp_eq"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_dsp_eq PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

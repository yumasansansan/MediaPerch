# SPDX-License-Identifier: GPL-3.0-or-later
#
# The third geometry: a stage that answers `configure` with a different channel
# count.
#
# The matrix is a library as well as a module so the tests can read the
# coefficients rather than infer them from what came out of a device.

add_library(mediaperch_mix STATIC mix.cpp)
target_include_directories(mediaperch_mix PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
# The matrix is written in the ABI's own speaker bits, so anything that reads
# the header reads those too.
target_link_libraries(mediaperch_mix PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)
set_target_properties(mediaperch_mix PROPERTIES FOLDER "modules")

add_library(mp_dsp_mix MODULE dsp_mix.cpp)
target_link_libraries(mp_dsp_mix
    PRIVATE MediaPerch::abi mediaperch_flags mediaperch_mix)

set_target_properties(mp_dsp_mix PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_dsp_mix"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_dsp_mix PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

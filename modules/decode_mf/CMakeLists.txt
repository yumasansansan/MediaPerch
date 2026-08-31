# SPDX-License-Identifier: GPL-3.0-or-later
#
# Media Foundation, which is already on the machine. Nothing to vendor and
# nothing to ship.

add_library(mp_decode_mf MODULE decode_mf.cpp)

target_link_libraries(mp_decode_mf PRIVATE MediaPerch::abi mediaperch_flags)
target_link_libraries(mp_decode_mf PRIVATE mfplat mfreadwrite mfuuid ole32 propsys)

set_target_properties(mp_decode_mf PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_mf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_mf PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# The only decoder here with no dependency of any kind: no submodule, no runtime
# library, no OS codec. See alac.hpp for why that was worth writing rather than
# linking the reference implementation.
#
# alac.cpp and mp4.cpp are also compiled into a small static library so that the
# fuzzers can drive the parsers without loading a module.

add_library(mediaperch_alac STATIC alac.cpp mp4.cpp)
target_include_directories(mediaperch_alac PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_alac PRIVATE mediaperch_flags)
set_target_properties(mediaperch_alac PROPERTIES FOLDER "modules")

add_library(mp_decode_alac MODULE decode_alac.cpp)

target_link_libraries(mp_decode_alac PRIVATE MediaPerch::abi mediaperch_flags mediaperch_alac)

set_target_properties(mp_decode_alac PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_alac"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_alac PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

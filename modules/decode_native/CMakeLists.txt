# SPDX-License-Identifier: GPL-3.0-or-later
#
# WAV and FLAC from two public-domain headers. Like every module it links the ABI
# header and nothing else of ours.

add_library(mp_decode_native MODULE
    decode_native.cpp
    dr_libs_impl.c
)

target_link_libraries(mp_decode_native PRIVATE MediaPerch::abi mediaperch_flags)
target_include_directories(mp_decode_native PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

# The dr_libs translation unit is somebody else's code and is held to somebody
# else's warning standard. decode_native.cpp keeps ours.
if(MSVC)
    set_source_files_properties(dr_libs_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_libs_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

set_target_properties(mp_decode_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_native"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_native PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

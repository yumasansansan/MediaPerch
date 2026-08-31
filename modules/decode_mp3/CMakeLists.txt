# SPDX-License-Identifier: GPL-3.0-or-later
#
# MP3 through dr_mp3, which is already in external/dr_libs beside the dr_wav and
# dr_flac that decode_native uses. No new submodule, and no new anything: the
# whole module is one source file plus the implementation unit.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/dr_libs/dr_mp3.h")
    message(WARNING
        "external/dr_libs is empty -- run `git submodule update --init`. "
        "Skipping decode_mp3.")
    return()
endif()

add_library(mp_decode_mp3 MODULE
    decode_mp3.cpp
    dr_mp3_impl.c
)

target_link_libraries(mp_decode_mp3 PRIVATE MediaPerch::abi mediaperch_flags)
target_include_directories(mp_decode_mp3 PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

# Without this dr_mp3 decodes to int16 and converts up on the way out, which
# would be a quantisation performed inside a decoder. MP3's synthesis filterbank
# produces real numbers; this makes float what is actually computed. It is set
# on the target so the implementation unit and the module agree about the size
# of drmp3's internal frame buffer -- disagreeing there is a silent overrun.
target_compile_definitions(mp_decode_mp3 PRIVATE DR_MP3_FLOAT_OUTPUT)

# Somebody else's parser, held to somebody else's warning standard.
if(MSVC)
    set_source_files_properties(dr_mp3_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_mp3_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

set_target_properties(mp_decode_mp3 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_mp3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_mp3 PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

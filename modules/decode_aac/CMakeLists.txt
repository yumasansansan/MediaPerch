# SPDX-License-Identifier: GPL-3.0-or-later
#
# AAC through FAAD2, from the external/faad2 submodule.
#
# LICENCE: FAAD2 is GPL-2.0-or-later, which is compatible with this project's
# GPL-3.0-or-later. It carries an attribution requirement under GPLv2 section
# 2c: the "appropriate copyright message" must read
#
#     Code from FAAD2 is copyright (c) Nero AG, www.nero.com
#
# which is why that sentence is in the module's user-visible name and shows up
# in `mediaperch-probe modules`. It is a condition of use, not a courtesy.
#
# FDK-AAC is the other obvious candidate and is not GPL-compatible at all, so it
# is not an option here regardless of quality.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/faad2/CMakeLists.txt")
    message(WARNING
        "external/faad2 is empty -- run `git submodule update --init`. "
        "Skipping decode_aac; the tree still builds without it.")
    return()
endif()

set(FAAD_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/faad2"
                 "${CMAKE_BINARY_DIR}/external/faad2" EXCLUDE_FROM_ALL)

add_library(mp_decode_aac MODULE decode_aac.cpp)

target_link_libraries(mp_decode_aac PRIVATE
    MediaPerch::abi mediaperch_flags mediaperch_mp4 faad)
target_include_directories(mp_decode_aac PRIVATE "${CMAKE_SOURCE_DIR}/external/faad2/include")

set_target_properties(mp_decode_aac PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_aac"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_aac PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

# Not ours, not held to our warning set.
foreach(_t IN ITEMS faad faad_static)
    if(TARGET ${_t})
        if(MSVC)
            target_compile_options(${_t} PRIVATE /W0)
        else()
            target_compile_options(${_t} PRIVATE -w)
        endif()
        set_target_properties(${_t} PROPERTIES FOLDER "external")
    endif()
endforeach()

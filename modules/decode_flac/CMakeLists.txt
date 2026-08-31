# SPDX-License-Identifier: GPL-3.0-or-later
#
# FLAC through libFLAC, built from the Xiph submodule.
#
# The submodule is why this is separate from `decode_native` rather than
# replacing it: one module has no dependencies at all and the other has a
# reference implementation, and which of those matters is the user's call, not
# ours. That is the whole point of the module boundary.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/flac/CMakeLists.txt")
    message(WARNING
        "external/flac is empty -- run `git submodule update --init`. "
        "Skipping decode_flac; the tree still builds without it.")
    return()
endif()

# libFLAC builds a great deal more than a decoder by default. None of it is
# wanted, and BUILD_TESTING in particular would attach a second test suite to
# this project's ctest.
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# Ogg FLAC would pull in a second submodule for a container almost nothing uses.
set(WITH_OGG OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/flac"
                 "${CMAKE_BINARY_DIR}/external/flac" EXCLUDE_FROM_ALL)

add_library(mp_decode_flac MODULE decode_flac.cpp)

target_link_libraries(mp_decode_flac PRIVATE MediaPerch::abi mediaperch_flags FLAC)

set_target_properties(mp_decode_flac PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_decode_flac"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_decode_flac PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

# libFLAC is not ours and is not held to our warning set.
if(TARGET FLAC)
    if(MSVC)
        target_compile_options(FLAC PRIVATE /W0)
    else()
        target_compile_options(FLAC PRIVATE -w)
    endif()
    set_target_properties(FLAC PROPERTIES FOLDER "external")
endif()

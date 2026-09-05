# SPDX-License-Identifier: GPL-3.0-or-later
#
# FLAC as a codec, on libFLAC from the Xiph submodule.
#
# The submodule is brought in here because this is the only thing in the tree
# that needs it: `demux_flac` reads the container with no library at all, so
# libFLAC is a *codec* dependency and nothing else. An install that does not
# build it still reads FLAC containers -- it just has nowhere to send the
# frames, and says so in those words.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/flac/CMakeLists.txt")
    message(WARNING
        "external/flac is empty -- run `git submodule update --init`. "
        "Skipping codec_flac; the tree still builds without it.")
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
# libFLAC's own Ogg support would read the container as well as the codec, which
# is the arrangement v2 exists to take apart: `demux_ogg` hands OggFLAC frames
# to this module already, and a second Ogg reader inside it would be a container
# in a place that is not allowed to have one.
set(WITH_OGG OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/flac"
                 "${CMAKE_BINARY_DIR}/external/flac" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_codec_flac
    KIND codec
    SOURCES codec_flac.cpp
    LINK FLAC)

# libFLAC is not ours and is not held to our warning set.
if(TARGET FLAC)
    if(MSVC)
        target_compile_options(FLAC PRIVATE /W0)
    else()
        target_compile_options(FLAC PRIVATE -w)
    endif()
    set_target_properties(FLAC PROPERTIES FOLDER "external")
endif()

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_codec_flac PRIVATE mediaperch_pcm_format)

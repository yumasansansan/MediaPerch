# SPDX-License-Identifier: GPL-3.0-or-later
#
# Opus on libopus itself rather than through opusfile, which is the container
# and the codec in one. This module has never heard of Ogg.
#
# opusfile is not in the tree at all any more. It went with `decode_ogg`, and
# with it went its HTTP support -- a decoder that can open a socket is a decoder
# with an attack surface it did not need, and turning that off was a setting
# somebody had to remember rather than a thing that was absent.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/opus/CMakeLists.txt")
    message(WARNING
        "external/opus is empty -- run `git submodule update --init`. "
        "Skipping codec_opus; the tree still builds without it.")
    return()
endif()

# The version libopus reports, pinned here beside the gitlink that decides it.
# cmake/OpusPackageVersion.cmake reads this instead of running `git describe`,
# which upstream does and which fails in any checkout without tags.
set(MEDIAPERCH_OPUS_VERSION "1.6.1")      # external/opus at v1.6.1

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/external/opus"
                 "${CMAKE_BINARY_DIR}/external/opus" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_codec_opus
    KIND codec
    SOURCES codec_opus.cpp
    LINK Opus::opus)

if(TARGET opus)
    if(MSVC)
        target_compile_options(opus PRIVATE /W0)
    else()
        target_compile_options(opus PRIVATE -w)
    endif()
    set_target_properties(opus PROPERTIES FOLDER "external")
endif()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vorbis on libvorbis itself rather than through vorbisfile, which is the
# container and the codec in one. This module has never heard of Ogg -- it takes
# the three header packets and then packets, from wherever they came.
#
# libvorbis asks `find_package(Ogg REQUIRED)`; cmake/FindOgg.cmake answers with
# the in-tree target, which modules/demux/ogg creates before this runs. That
# ordering is the one thing this file depends on.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/vorbis/CMakeLists.txt" OR NOT TARGET Ogg::ogg)
    message(STATUS "libvorbis or libogg is not configured -- skipping codec_vorbis")
    return()
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/external/vorbis"
                 "${CMAKE_BINARY_DIR}/external/vorbis" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_codec_vorbis
    KIND codec
    SOURCES codec_vorbis.cpp
    LINK vorbis Ogg::ogg)

# vorbisenc and vorbisfile are built because they are part of that project, and
# linked by nothing here: the encoder has no caller and the file reader is a
# container, which is somebody else's job now.
foreach(_ext IN ITEMS vorbis vorbisenc vorbisfile)
    if(TARGET ${_ext})
        if(MSVC)
            target_compile_options(${_ext} PRIVATE /W0)
        else()
            target_compile_options(${_ext} PRIVATE -w)
        endif()
        set_target_properties(${_ext} PROPERTIES FOLDER "external")
    endif()
endforeach()

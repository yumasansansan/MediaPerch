# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ogg as a container and nothing else, on libogg.
#
# **libogg is brought in here because Ogg is what this reads.** Under v1 one
# module pulled in four Xiph submodules -- ogg, vorbis, opus and opusfile --
# because it was the container and both codecs at once. Now each library sits
# with the module that needs it: libogg here, libvorbis with `codec_vorbis`,
# libopus with `codec_opus`, and **opusfile with nobody**, because opusfile is
# the container and the codec in one object and that is the arrangement this
# split exists to take apart. It was removed from the tree with `decode_ogg`.
#
# This directory is added before modules/codec/vorbis, because libvorbis asks
# `find_package(Ogg REQUIRED)` and cmake/FindOgg.cmake answers with the target
# the add_subdirectory below creates.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/ogg/CMakeLists.txt")
    message(WARNING
        "external/ogg is empty -- run `git submodule update --init`. "
        "Skipping demux_ogg; the tree still builds without it.")
    return()
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/ogg"
                 "${CMAKE_BINARY_DIR}/external/ogg" EXCLUDE_FROM_ALL SYSTEM)

mediaperch_add_module(mp_demux_ogg
    KIND demux
    SOURCES demux_ogg.cpp
    LINK Ogg::ogg)

if(TARGET ogg)
    if(MSVC)
        target_compile_options(ogg PRIVATE /W0)
    else()
        target_compile_options(ogg PRIVATE -w)
    endif()
    set_target_properties(ogg PROPERTIES FOLDER "external")
endif()

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_ogg PRIVATE mediaperch_module_log)

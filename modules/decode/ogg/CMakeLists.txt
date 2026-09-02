# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vorbis and Opus, from Xiph's own libraries, in one module.
#
# One module rather than two because these two codecs are not independent here:
# both arrive inside an Ogg stream, so both need libogg, and the Ogg-to-WAVE
# channel-order permutation below is *literally the same table* for each -- Opus
# mapping family 1 is defined as Vorbis order. Splitting them would duplicate
# libogg, duplicate that table, and buy a granularity nobody asked for, since
# a file that is one is never the other.
#
# It is called decode_ogg and not decode_xiph because FLAC is Xiph too, and
# because what this module actually knows is the container.

set(_ogg_deps
    "${CMAKE_SOURCE_DIR}/external/ogg/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/external/vorbis/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/external/opus/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/external/opusfile/CMakeLists.txt")
foreach(_dep IN LISTS _ogg_deps)
    if(NOT EXISTS "${_dep}")
        message(WARNING
            "${_dep} is missing -- run `git submodule update --init`. "
            "Skipping decode_ogg; the tree still builds without it.")
        return()
    endif()
endforeach()

# Order matters and is not incidental: cmake/FindOgg.cmake and
# cmake/FindOpus.cmake answer `find_package` with the targets these
# add_subdirectory calls create, so each library has to exist before the library
# that asks for it.
# opusfile has no option to skip its install rules, and its install(EXPORT)
# fails outright when ogg and opus are in-tree targets rather than imported ones
# -- an export set cannot reference a target that is not itself exported. Nothing
# in this subtree is installed, so the rules are simply not generated. The
# variable is deliberately not CACHE: it applies to this directory and the
# add_subdirectory calls below it, and nowhere else.
set(CMAKE_SKIP_INSTALL_RULES ON)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/ogg"
                 "${CMAKE_BINARY_DIR}/external/ogg" EXCLUDE_FROM_ALL)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/vorbis"
                 "${CMAKE_BINARY_DIR}/external/vorbis" EXCLUDE_FROM_ALL)

# The version each of these reports, pinned here beside the gitlink that decides
# it. cmake/OpusPackageVersion.cmake and cmake/OpusFilePackageVersion.cmake read
# these instead of running , which upstream does and which fails in
# any checkout without tags. Update them when the submodule moves.
set(MEDIAPERCH_OPUS_VERSION "1.6.1")      # external/opus at v1.6.1
set(MEDIAPERCH_OPUSFILE_VERSION "0.12")   # external/opusfile at v0.12-41-g3ecc22a

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/external/opus"
                 "${CMAKE_BINARY_DIR}/external/opus" EXCLUDE_FROM_ALL)

# OP_DISABLE_HTTP is the important one: opusfile's URL support pulls in OpenSSL,
# for a feature a local file player has no use for. A decoder that can open a
# socket is a decoder with an attack surface it did not need.
set(OP_DISABLE_HTTP ON CACHE BOOL "" FORCE)
set(OP_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(OP_DISABLE_DOCS ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/external/opusfile"
                 "${CMAKE_BINARY_DIR}/external/opusfile" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_decode_ogg
    KIND decode
    SOURCES decode_ogg.cpp
    LINK vorbisfile vorbis Ogg::ogg OpusFile::opusfile)

# None of these are ours, and none are held to our warning set.
foreach(_ext IN ITEMS ogg vorbis vorbisenc vorbisfile opus opusfile)
    if(TARGET ${_ext})
        if(MSVC)
            target_compile_options(${_ext} PRIVATE /W0)
        else()
            target_compile_options(${_ext} PRIVATE -w)
        endif()
        set_target_properties(${_ext} PROPERTIES FOLDER "external")
    endif()
endforeach()

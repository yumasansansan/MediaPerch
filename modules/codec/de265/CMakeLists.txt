# SPDX-License-Identifier: GPL-3.0-or-later
#
# libde265, and the reason HEVC needed a decoder of its own.
#
# **`codec_mft` was not an HEVC decoder, it was a hope that the machine had
# one.** Measured on the machine this was written on: the only Media Foundation
# transform that answered for HEVC was `HEVCVideoExtension`, which is a Store
# package, absent from a clean Windows, and not something a player may ship.
# H.264 has the Microsoft decoder in the box; HEVC has nothing. So a player that
# wants to open an HEVC file on somebody else's computer brings its own.
#
# **LGPL-3.0, which is why it can be here.** The library is LGPL-3.0 and the
# sample applications are MIT. This tree is GPL-3.0-or-later and LGPL-3.0 code
# may be linked into that, so the licences are not the obstacle they are for
# some of the alternatives -- and the module is a DLL of its own, which is what
# §4's "nothing is allocated across the module boundary" makes into a real
# boundary rather than a diagram.
#
# **An external project, though libde265 builds with CMake**, for the reason
# `modules/codec/aom/CMakeLists.txt` sets out at length: nesting a submodule's
# own CMake inside this tree's makes this tree responsible for the languages,
# runtimes and options that submodule enables, and the cascade that follows is
# worse than one extra CMake invocation. libde265 needs no assembler, so this is
# the cheapest external here -- no NASM, no Meson, nothing but a compiler.
#
# One release build rather than one per configuration, and the module pinned to
# the matching runtime. `modules/codec/dav1d/CMakeLists.txt` records why that is
# safe and not merely convenient.

set(de265_src "${CMAKE_SOURCE_DIR}/external/libde265")

if(NOT EXISTS "${de265_src}/CMakeLists.txt")
    message(WARNING
        "external/libde265 is missing -- run `git submodule update --init`. "
        "Skipping codec_de265; the tree still builds and simply has no HEVC "
        "decoder of its own, which on most machines means no HEVC at all.")
    return()
endif()

include(ExternalProject)

set(de265_prefix "${CMAKE_BINARY_DIR}/external/libde265")
set(de265_install "${de265_prefix}/install")
# libde265 keeps the `lib` prefix on Windows -- `libde265.lib`, not `de265.lib`
# -- which MSVC links perfectly well and CMake will not guess at.
set(de265_library "${de265_install}/lib/libde265.lib")

ExternalProject_Add(de265_external
    SOURCE_DIR "${de265_src}"
    BINARY_DIR "${de265_prefix}/build"
    INSTALL_DIR "${de265_install}"
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=${de265_install}"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        # Static, because the point of a module is that it is one file. The
        # sample applications and the decoder tests are the parts that are MIT
        # rather than LGPL and are of no use here either way.
        "-DBUILD_SHARED_LIBS=OFF"
        "-DENABLE_SDL=OFF"
        "-DENABLE_DECODER=OFF"
        "-DENABLE_ENCODER=OFF"
    BUILD_BYPRODUCTS "${de265_library}"
    # The submodule can move under an external project and ExternalProject does
    # not look -- see modules/codec/dav1d/CMakeLists.txt, where that cost a set
    # of measurements taken against the wrong commit.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

add_library(de265::de265 STATIC IMPORTED GLOBAL)
set_target_properties(de265::de265 PROPERTIES
    IMPORTED_LOCATION "${de265_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${de265_install}/include"
    # The static library exports nothing, and libde265's header decorates its
    # declarations for a DLL unless told otherwise.
    INTERFACE_COMPILE_DEFINITIONS "LIBDE265_STATIC_BUILD")
# The include directory does not exist until the external project installs, and
# CMake refuses an INTERFACE include path that is not there yet.
file(MAKE_DIRECTORY "${de265_install}/include")
add_dependencies(de265::de265 de265_external)

mediaperch_add_module(mp_codec_de265
    KIND codec
    SOURCES codec_de265.cpp
    # `hvcC` to Annex B, which is `avcC` to Annex B with a different record in
    # front of it -- see modules/shared/h264, where the emitter never knew which.
    #
    # **The source, not the archive.** This module is pinned to the release
    # runtime below, and libde265 is C++, so every object in this DLL has to
    # carry the same runtime; the `mediaperch_h264` archive follows the
    # configuration, and a Debug build of it fails the link with a
    # RuntimeLibrary mismatch -- which is exactly what the Debug CI job did.
    LINK de265::de265 mediaperch_h264_sources mediaperch_decoder_threads)

set_target_properties(mp_codec_de265 PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

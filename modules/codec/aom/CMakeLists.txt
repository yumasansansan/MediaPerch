# SPDX-License-Identifier: GPL-3.0-or-later
#
# libaom, the AV1 reference, and why a second AV1 decoder is worth its build
# time.
#
# **It is not here to decode faster than dav1d.** It is slower, by a lot, and
# dav1d exists precisely because the reference is not a player's decoder. What
# libaom is, and dav1d is not, is the **reference implementation**: AV1's
# decoding process is defined bit-exactly, so libaom and dav1d must agree on
# every sample of every frame or one of them is wrong.
#
# That is a far stronger check than any this tree can make on a video decoder on
# its own. `codec_dav1d_test.cpp` can say a frame has a spread of luminance and
# more than one hue, which rules out a cleared buffer and not much else; two
# independent decoders agreeing byte for byte rules out almost everything. It is
# the method §12 already uses for audio -- one decoder against another, and then
# both against what was encoded -- arriving for video.
#
# **An external project, though libaom builds with CMake, and that is the
# interesting part.** `add_subdirectory` looks obviously right and is not:
# libaom calls `enable_language(ASM_NASM)` from inside its own tree, and CMake
# requires that call to happen in the highest directory common to every target
# using the language. Nested, it generates a build with no rule to assemble
# anything:
#
#     Missing variable is: CMAKE_ASM_NASM_COMPILE_OBJECT   (nine times)
#
# Hoisting the call to this tree's top level fixes that one and starts a
# cascade, because two other submodules do the same thing in their own corners:
# external/wavpack enables ASM_MASM and external/mpg123's `project` line asks
# for ASM. Enabling those at the top level too gets as far as
#
#     MSVC_RUNTIME_LIBRARY value 'MultiThreadedDebugDLL' not known for this ASM
#     compiler
#
# from inside wavpack -- which had been configuring perfectly well until this
# tree started enabling languages on its behalf.
#
# The same libaom, configured standalone, works under both Ninja generators
# first time. So the fault is the nesting and nothing else, and the answer is
# not to nest: libaom gets its own CMake invocation, its own assembler and its
# own runtime setting, and this tree's language configuration stays exactly as
# it was. external/dav1d is already an external project for a different reason,
# so the shape is not a new one here.
#
# It needs NASM, which external/dav1d's Meson build already required, so this
# adds no tool -- see modules/codec/dav1d/CMakeLists.txt for where it comes from
# on the development machine.

set(aom_src "${CMAKE_SOURCE_DIR}/external/aom")

if(NOT EXISTS "${aom_src}/CMakeLists.txt")
    message(WARNING
        "external/aom is missing -- run `git submodule update --init`. Skipping "
        "codec_aom; the tree still builds, dav1d still decodes AV1, and what is "
        "lost is the reference to check it against.")
    return()
endif()

# Named rather than searched for: enabling ASM_NASM makes CMake take the first
# assembler it finds, and on the development machine that is the YASM inside a
# Perl distribution -- identified as YASM, with no compile rule, and the same
# nine errors with no hint about which program it meant.
find_program(MEDIAPERCH_NASM NAMES nasm nasm.exe
    DOC "NASM, which assembles libaom. external/dav1d's Meson build needs it too.")

if(NOT MEDIAPERCH_NASM)
    message(WARNING
        "nasm is not on PATH and libaom's x86 code needs an assembler. Skipping "
        "codec_aom -- the tree still builds and dav1d still decodes AV1.")
    return()
endif()

include(ExternalProject)

set(aom_prefix "${CMAKE_BINARY_DIR}/external/aom")
set(aom_install "${aom_prefix}/install")
set(aom_library "${aom_install}/lib/aom.lib")

ExternalProject_Add(aom_external
    SOURCE_DIR "${aom_src}"
    BINARY_DIR "${aom_prefix}/build"
    INSTALL_DIR "${aom_install}"
    # Single-config Ninja, because one release build is what is wanted: nothing
    # here steps through libaom, and building the reference once per
    # configuration would be minutes each for no reader. The module is pinned to
    # the matching runtime below, which §4's rule that nothing is allocated
    # across the module boundary is what makes safe -- the same argument
    # external/dav1d's build records at more length.
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=${aom_install}"
        "-DCMAKE_ASM_NASM_COMPILER=${MEDIAPERCH_NASM}"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
        "-DCONFIG_AV1_DECODER=1"
        "-DCONFIG_AV1_ENCODER=0"
        "-DENABLE_EXAMPLES=OFF"
        "-DENABLE_TOOLS=OFF"
        "-DENABLE_TESTS=OFF"
        "-DENABLE_TESTDATA=OFF"
        "-DENABLE_DOCS=OFF"
    BUILD_BYPRODUCTS "${aom_library}"
    # The submodule can move under an external project and ExternalProject does
    # not look -- see modules/codec/dav1d/CMakeLists.txt, where that cost a set
    # of measurements taken against the wrong commit.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

add_library(aom::aom STATIC IMPORTED GLOBAL)
set_target_properties(aom::aom PROPERTIES
    IMPORTED_LOCATION "${aom_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${aom_install}/include")
# The include directory does not exist until the external project installs, and
# CMake refuses an INTERFACE include path that is not there yet.
file(MAKE_DIRECTORY "${aom_install}/include")
add_dependencies(aom::aom aom_external)

mediaperch_add_module(mp_codec_aom
    KIND codec
    SOURCES codec_aom.cpp
    LINK aom::aom)

set_target_properties(mp_codec_aom PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

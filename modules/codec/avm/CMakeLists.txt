# SPDX-License-Identifier: GPL-3.0-or-later
#
# avm, the AV2 reference, and the only decoder AV2 has.
#
# **AV2 reached 1.0.0 on 2026-05-29** -- one released version, and a CHANGELOG
# with one line in it. dav2d exists and is where a player's AV2 decoder will
# come from, the way dav1d is for AV1; it is a submodule here already and has no
# module yet. So this is not a second opinion the way `codec_aom` is: it is the
# whole of AV2 support until dav2d is ready, and a breaking change in a
# 1.0.0-shaped research codebase is a thing to expect rather than to be
# surprised by. The submodule is pinned to the tag for exactly that reason.
#
# **An external project, for the reason codec_aom records at length.** avm is a
# fork of libaom and inherits the same `enable_language(ASM_NASM)` from inside
# its own tree, which CMake requires to happen in the highest directory common
# to every target using the language. Nested, it generates a build with no rule
# to assemble anything; hoisting the call starts a cascade through
# external/wavpack and external/mpg123. Standalone it works first time -- see
# modules/codec/aom/CMakeLists.txt, where that took three tries to establish.
#
# **The encoder is off, and that is what keeps a media player out of the
# TensorFlow business.** avm's ML tools -- CONFIG_ML_PART_SPLIT for partition
# search and CONFIG_DIP_EXT_PRUNING for intra mode pruning -- are on by default
# and force CONFIG_TENSORFLOW_LITE on with them, which makes the build extract
# and patch sixteen vendored tarballs (TensorFlow Lite, abseil, XNNPACK,
# protobuf, Eigen and the rest) through `bash -c tar` and `patch -p1`. Every one
# of those is encoder-side: the whole TFLite block sits inside
# `if(CONFIG_AV2_ENCODER)`, and what DIP_EXT_PRUNING adds to the decoder is one
# extra output from the intra predictor -- eleven downsampled edge pixels the
# encoder's model reads and the decoder does not. So the tools stay at their
# defaults, exactly as the reference defines them, and turning the encoder off
# is the whole of the difference. Measured: 1.3 minutes to build, no bash, no
# tar, no network.

set(avm_src "${CMAKE_SOURCE_DIR}/external/avm")

if(NOT EXISTS "${avm_src}/CMakeLists.txt")
    message(WARNING
        "external/avm is missing -- run `git submodule update --init`. Skipping "
        "codec_avm; the tree still builds and simply has no AV2 decoder, which "
        "at present means no AV2 at all.")
    return()
endif()

# Named rather than searched for: enabling ASM_NASM makes CMake take the first
# assembler it finds, and avm asks for `yasm` before `nasm` outright --
# `find_program(CMAKE_ASM_NASM_COMPILER yasm ...)`. On the development machine
# the first yasm on PATH is the one inside a Perl distribution. Setting the
# cache variable here makes that `find_program` a no-op. codec_aom records the
# same trap and the nine errors it produces.
find_program(MEDIAPERCH_NASM NAMES nasm nasm.exe
    DOC "NASM, which assembles avm. external/aom and external/dav1d need it too.")

if(NOT MEDIAPERCH_NASM)
    message(WARNING
        "nasm is not on PATH and avm's x86 code needs an assembler. Skipping "
        "codec_avm -- the tree still builds and simply has no AV2 decoder.")
    return()
endif()

include(ExternalProject)

set(avm_prefix "${CMAKE_BINARY_DIR}/external/avm")
# **No install step, because avm has none here.** `setup_avm_install_targets`
# opens with `if(NOT (MSVC OR XCODE))`, so under MSVC the build produces its
# library and nothing else. The library is taken from the build directory and
# the headers from the source tree, which is what modules/codec/vpx already
# does for the same reason.
set(avm_library "${avm_prefix}/avm.lib")

ExternalProject_Add(avm_external
    SOURCE_DIR "${avm_src}"
    BINARY_DIR "${avm_prefix}"
    # Single-config Ninja and one release build, for the reason
    # modules/codec/aom gives: nothing here steps through avm, and building a
    # reference decoder once per configuration would be minutes each for no
    # reader. The module is pinned to the matching runtime below, which §4's
    # rule that nothing is allocated across the module boundary is what makes
    # safe.
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_ASM_NASM_COMPILER=${MEDIAPERCH_NASM}"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
        "-DCONFIG_AV2_DECODER=1"
        "-DCONFIG_AV2_ENCODER=0"
        # Matroska is a demuxer in this tree, so avm's own muxer is not wanted --
        # and it is what would drag in third_party/libwebm.
        "-DCONFIG_WEBM_IO=0"
        "-DENABLE_EXAMPLES=OFF"
        "-DENABLE_TOOLS=OFF"
        "-DENABLE_TESTS=OFF"
        "-DENABLE_TESTDATA=OFF"
        "-DENABLE_DOCS=OFF"
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E true
    BUILD_BYPRODUCTS "${avm_library}"
    # The submodule can move under an external project and ExternalProject does
    # not look -- see modules/codec/dav1d/CMakeLists.txt, where that cost a set
    # of measurements taken against the wrong commit.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)

add_library(avm::avm STATIC IMPORTED GLOBAL)
set_target_properties(avm::avm PROPERTIES
    IMPORTED_LOCATION "${avm_library}"
    # avm's public headers are `avm/avm_decoder.h` and friends, at the root of
    # its own tree, and they include only each other -- the generated
    # `avm_config.h` is internal.
    INTERFACE_INCLUDE_DIRECTORIES "${avm_src}")
add_dependencies(avm::avm avm_external)

mediaperch_add_module(mp_codec_avm
    KIND codec
    SOURCES codec_avm.cpp
    LINK avm::avm)

set_target_properties(mp_codec_avm PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

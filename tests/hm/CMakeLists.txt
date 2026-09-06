# SPDX-License-Identifier: GPL-3.0-or-later
#
# HM, the HEVC reference decoder, built as the program it is.
#
# **HM has no library API, and that is the whole design decision here.**
# `TDecTop::decode` is driven by about two hundred and fifty lines of state in
# `TAppDecTop` -- `bNewPicture`, `executeLoopFilters`, POC tracking, an output
# routine that walks a `TComList<TComPic*>` -- and `TAppDecTop::decode()` reads
# its bitstream from a file. What HM ships to be used is a program. Wrapping
# `TLibDecoder` in a module would mean reimplementing that state machine and
# then maintaining it against upstream, and the result would be a decoder with
# HM's name that HM's authors had never run.
#
# So it is used as a program, which is also the only way it is *the reference*:
# what the cross-check compares against is what ITU/ISO/IEC published, driven
# the way they drive it. `format_matrix` already runs external tools for the
# same reason.
#
# **What it is for.** HEVC's decoding process is defined bit-exactly, so
# libde265 and HM must agree on every sample of every frame or one of them is
# wrong. That is a far stronger check than anything a single decoder can be
# held to -- `codec_de265_test.cpp` can say a frame has more than one luma
# value, which rules out a cleared buffer and not much else. It is §12's method
# for audio, and `codec_aom`'s for AV1, arriving for HEVC.
#
# **Licence.** BSD-3-Clause from ITU/ISO/IEC, with a preamble worth reading
# before shipping anything built from it: "This software may be subject to
# other third party and contributor rights, including patent rights, and no
# such rights are granted under this license." That is a patent notice about
# HEVC rather than a restriction on HM, and this builds HM for a test rather
# than shipping it.

set(hm_src "${CMAKE_SOURCE_DIR}/external/HM")

if(NOT EXISTS "${hm_src}/CMakeLists.txt")
    message(STATUS
        "external/HM is missing -- run `git submodule update --init`. The HEVC "
        "cross-check is skipped; codec_de265 is still tested against the "
        "container and against itself.")
    return()
endif()

include(ExternalProject)

set(hm_prefix "${CMAKE_BINARY_DIR}/external/HM")
# Not where HM puts it -- see find_decoder.cmake, which is why there is a copy
# step at all -- but where everything downstream can name it.
set(hm_bin "${hm_prefix}/TAppDecoder.exe")

ExternalProject_Add(hm_external
    SOURCE_DIR "${hm_src}"
    BINARY_DIR "${hm_prefix}/build"
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        # **HM turns warnings into errors for MSVC**, in its own
        # `bb_enable_warnings`, against a compiler generations newer than the
        # one it was written for. This is somebody else's code built for one
        # purpose; the warnings in it are not this tree's to fix, and /WX- after
        # their /WX is how that is said.
        "-DCMAKE_CXX_FLAGS=/WX- /wd4996 /wd4267 /wd4244 /wd4018"
    # Only the decoder. HM builds an encoder, two analysers and three utilities
    # besides, and none of them is what the reference is being asked for.
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${hm_prefix}/build" --target TAppDecoder
    INSTALL_COMMAND "${CMAKE_COMMAND}"
        "-Dsrc=${hm_src}" "-Ddest=${hm_bin}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/find_decoder.cmake"
    BUILD_BYPRODUCTS "${hm_bin}"
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)

set(MEDIAPERCH_HM_DECODER "${hm_bin}" PARENT_SCOPE)
set(MEDIAPERCH_HM_FOUND ON PARENT_SCOPE)

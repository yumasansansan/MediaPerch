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

# The library, told the instruction set, as the fuzz tree builds it too:
# cmake/Opus.cmake.
include(Opus)

mediaperch_add_module(mp_codec_opus
    KIND codec
    SOURCES codec_opus.cpp
    LINK Opus::opus)

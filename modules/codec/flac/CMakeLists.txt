# SPDX-License-Identifier: GPL-3.0-or-later
#
# FLAC as a codec, on libFLAC. The submodule is brought in by
# modules/decode/flac, which is added first; that ordering goes away with
# `decode_flac` itself, and the `add_subdirectory` moves here when it does.

if(NOT TARGET FLAC)
    message(STATUS "libFLAC is not configured -- skipping codec_flac")
    return()
endif()

mediaperch_add_module(mp_codec_flac
    KIND codec
    SOURCES codec_flac.cpp
    LINK FLAC)

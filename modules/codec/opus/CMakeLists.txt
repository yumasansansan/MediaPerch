# SPDX-License-Identifier: GPL-3.0-or-later
#
# Opus on libopus itself rather than through opusfile, which is the container
# and the codec in one. This module has never heard of Ogg.

if(NOT TARGET Opus::opus)
    message(STATUS "libopus is not configured -- skipping codec_opus")
    return()
endif()

mediaperch_add_module(mp_codec_opus
    KIND codec
    SOURCES codec_opus.cpp
    LINK Opus::opus)

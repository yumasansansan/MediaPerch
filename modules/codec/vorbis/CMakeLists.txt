# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vorbis on libvorbis itself rather than through vorbisfile, which is the
# container and the codec in one. This module has never heard of Ogg -- it takes
# the three header packets and then packets, from wherever they came.

if(NOT TARGET vorbis)
    message(STATUS "libvorbis is not configured -- skipping codec_vorbis")
    return()
endif()

mediaperch_add_module(mp_codec_vorbis
    KIND codec
    SOURCES codec_vorbis.cpp
    LINK vorbis Ogg::ogg)

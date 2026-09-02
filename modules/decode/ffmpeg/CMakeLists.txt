# SPDX-License-Identifier: GPL-3.0-or-later
#
# FFmpeg is not vendored, not submoduled and not linked: this module finds
# ffmpeg and ffprobe at run time and declines every file when they are absent.
# So there is nothing here to build against -- which is the point.

mediaperch_add_module(mp_decode_ffmpeg
    KIND decode
    SOURCES decode_ffmpeg.cpp)


# SPDX-License-Identifier: GPL-3.0-or-later
#
# FFmpeg is not vendored, not submoduled and not linked: this module finds
# ffmpeg and ffprobe at run time and declines every file when they are absent.
# So there is nothing here to build against -- which is the point.

mediaperch_add_module(mp_demux_ffmpeg
    KIND demux
    SOURCES demux_ffmpeg.cpp)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_ffmpeg PRIVATE mediaperch_module_log)

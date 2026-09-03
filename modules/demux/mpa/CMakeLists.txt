# SPDX-License-Identifier: GPL-3.0-or-later
#
# MPEG audio as a container, on libmpg123.
#
# The submodule is brought in by modules/codec/mpa, which the root CMakeLists
# adds first -- the same arrangement libFLAC has, where the codec owns the
# dependency and the demuxer links the same target and decodes nothing with it.

if(NOT TARGET libmpg123)
    message(WARNING
        "libmpg123 is not available -- skipping demux_mpa. "
        "MPEG audio falls to demux_ffmpeg.")
    return()
endif()

mediaperch_add_module(mp_demux_mpa
    KIND demux
    SOURCES demux_mpa.cpp
    LINK libmpg123)

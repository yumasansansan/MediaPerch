# SPDX-License-Identifier: GPL-3.0-or-later
#
# MP4 as a container and nothing else -- the first module of ABI v2, and the one
# that proves the shape with the least new code, because the parser it uses was
# already shared between two decoders that each carried half of this.

mediaperch_add_module(mp_demux_mp4
    KIND demux
    SOURCES demux_mp4.cpp
    LINK mediaperch_mp4)

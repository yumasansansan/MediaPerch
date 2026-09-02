# SPDX-License-Identifier: GPL-3.0-or-later
#
# MPEG audio's framing, written here. No library at all: the frame header is
# four bytes and the LAME tag is a table, and both are what a container reader
# is for.

mediaperch_add_module(mp_demux_mpeg
    KIND demux
    SOURCES demux_mpeg.cpp)

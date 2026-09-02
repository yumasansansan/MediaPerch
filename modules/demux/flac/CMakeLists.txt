# SPDX-License-Identifier: GPL-3.0-or-later
#
# The native FLAC container, on this tree's own framing code and nothing else.
# No submodule: `demux_flac` plus `codec_flac_dr` is a FLAC reader for an install
# that builds none of them, which is the promise `decode_native` made.

mediaperch_add_module(mp_demux_flac
    KIND demux
    SOURCES demux_flac.cpp
    LINK mediaperch_flacframe)

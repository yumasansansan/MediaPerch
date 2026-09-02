# SPDX-License-Identifier: GPL-3.0-or-later
#
# The native FLAC container, on this tree's own framing code and nothing else.
# No submodule, which is worth having on its own: the container is read the same
# way whatever decodes the frames, so a second FLAC codec would drop in beside
# codec_flac without this file being touched.

mediaperch_add_module(mp_demux_flac
    KIND demux
    SOURCES demux_flac.cpp
    LINK mediaperch_flacframe)

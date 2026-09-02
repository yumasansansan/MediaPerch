# SPDX-License-Identifier: GPL-3.0-or-later
#
# ADTS framing, written here. No library: the header is seven bytes and the
# AudioSpecificConfig it implies is two.

mediaperch_add_module(mp_demux_adts
    KIND demux
    SOURCES demux_adts.cpp)

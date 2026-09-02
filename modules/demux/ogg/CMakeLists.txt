# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ogg as a container and nothing else, on libogg -- which this tree already
# vendors separately from the codecs, because Xiph ships them that way. That is
# why this is the split that costs least and shows most.
#
# It depends on modules/decode/ogg having been added first, which is where the
# Xiph submodules are brought in. That dependency goes when decode_ogg does.

if(NOT TARGET Ogg::ogg)
    message(STATUS "libogg is not configured -- skipping demux_ogg")
    return()
endif()

mediaperch_add_module(mp_demux_ogg
    KIND demux
    SOURCES demux_ogg.cpp
    LINK Ogg::ogg)

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Media Foundation, which is already on the machine. Nothing to vendor and
# nothing to ship.

mediaperch_add_module(mp_demux_mf
    KIND demux
    SOURCES demux_mf.cpp)

target_link_libraries(mp_demux_mf PRIVATE mfplat mfreadwrite mfuuid ole32 propsys)


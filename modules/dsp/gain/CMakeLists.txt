# SPDX-License-Identifier: GPL-3.0-or-later
#
# The first MpDspVtbl. A shape with one implementation is a guess; this is what
# makes it an interface.

mediaperch_add_module(mp_dsp_gain
    KIND dsp
    SOURCES dsp_gain.cpp)


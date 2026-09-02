# SPDX-License-Identifier: GPL-3.0-or-later
#
# The equaliser. The sections themselves live in modules/biquad, because the
# loudness meter wants the same arithmetic for a cascade nobody chose.

mediaperch_add_module(mp_dsp_eq
    KIND dsp
    SOURCES dsp_eq.cpp autoeq.cpp
    LINK mediaperch_biquad mediaperch_convolve mediaperch_transform)


# SPDX-License-Identifier: GPL-3.0-or-later
#
# Convolution with an impulse response somebody measured. The engine is
# modules/convolve -- the same one the equaliser's FIR modes run on -- so what
# is built here is only the part that reads a file and makes it agree with the
# stream: its rate, its channels, its length and its gain.
#
# It links the resampler, which is not an accident: an impulse measured at
# 44.1 kHz is the wrong filter for a 96 kHz stream, and converting it is the
# same decision the equaliser makes when it re-derives its biquads at each rate.

add_library(mediaperch_impulse STATIC impulse.cpp)
target_include_directories(mediaperch_impulse PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
# dr_wav's implementation comes from modules/shared/dr_wav, compiled once and
# held to nobody's warning standard but its own; impulse.cpp keeps ours.
target_link_libraries(mediaperch_impulse
    PUBLIC mediaperch_resample PRIVATE mediaperch_flags mediaperch_dr_wav)
set_target_properties(mediaperch_impulse PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_dsp_convolve
    KIND dsp
    SOURCES dsp_convolve.cpp
    LINK mediaperch_convolve mediaperch_impulse)


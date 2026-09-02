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

add_library(mediaperch_impulse STATIC impulse.cpp dr_wav_impl.c)
target_include_directories(mediaperch_impulse
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")
target_link_libraries(mediaperch_impulse
    PUBLIC mediaperch_resample PRIVATE mediaperch_flags)
set_target_properties(mediaperch_impulse PROPERTIES FOLDER "modules")

# Somebody else's code, held to somebody else's warning standard.
if(MSVC)
    set_source_files_properties(dr_wav_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_wav_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

mediaperch_add_module(mp_dsp_convolve
    KIND dsp
    SOURCES dsp_convolve.cpp
    LINK mediaperch_convolve mediaperch_impulse)


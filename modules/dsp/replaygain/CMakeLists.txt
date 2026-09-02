# SPDX-License-Identifier: GPL-3.0-or-later
#
# ReplayGain, measured rather than read. The meter is a library because the
# probe's `loudness` command is the same meter run over a whole file, and
# because a loudness number nobody can reproduce is a loudness number nobody
# should trust.

add_library(mediaperch_loudness STATIC loudness.cpp)
target_include_directories(mediaperch_loudness PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_loudness
    PUBLIC MediaPerch::abi mediaperch_biquad PRIVATE mediaperch_flags)
set_target_properties(mediaperch_loudness PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_dsp_replaygain
    KIND dsp
    SOURCES dsp_replaygain.cpp
    LINK mediaperch_loudness)


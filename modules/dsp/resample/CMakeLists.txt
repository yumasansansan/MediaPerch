# SPDX-License-Identifier: GPL-3.0-or-later
#
# The stage MpDspVtbl was shaped for: one that answers `configure` with a
# different sample rate.
#
# The filter is a library as well as a module so the tests can measure its
# response without loading anything -- the same arrangement as codec_aac, and
# for the same reason: a filter that is only ever tested through a device is a
# filter nobody can put a number on.

add_library(mediaperch_resample STATIC resample.cpp design.cpp)
target_include_directories(mediaperch_resample PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_resample
    PUBLIC mediaperch_transform PRIVATE mediaperch_flags)
set_target_properties(mediaperch_resample PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_dsp_resample
    KIND dsp
    SOURCES dsp_resample.cpp
    LINK mediaperch_resample)


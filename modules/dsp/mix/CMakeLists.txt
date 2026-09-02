# SPDX-License-Identifier: GPL-3.0-or-later
#
# The third geometry: a stage that answers `configure` with a different channel
# count.
#
# The matrix is a library as well as a module so the tests can read the
# coefficients rather than infer them from what came out of a device.

add_library(mediaperch_mix STATIC mix.cpp)
target_include_directories(mediaperch_mix PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
# The matrix is written in the ABI's own speaker bits, so anything that reads
# the header reads those too.
target_link_libraries(mediaperch_mix PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)
set_target_properties(mediaperch_mix PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_dsp_mix
    KIND dsp
    SOURCES dsp_mix.cpp
    LINK mediaperch_mix)


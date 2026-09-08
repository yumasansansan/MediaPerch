# SPDX-License-Identifier: GPL-3.0-or-later
#
# The equaliser. The sections themselves live in modules/biquad, because the
# loudness meter wants the same arithmetic for a cascade nobody chose.

mediaperch_add_module(mp_dsp_eq
    KIND dsp
    SOURCES dsp_eq.cpp autoeq.cpp
    LINK mediaperch_biquad mediaperch_convolve mediaperch_transform)


# win_path.hpp, for a path past MAX_PATH: the one header beside module_log
# that every module opening a file by name includes (see it for why).
target_include_directories(mp_dsp_eq PRIVATE "${CMAKE_SOURCE_DIR}/modules/shared/module_log")

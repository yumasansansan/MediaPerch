# SPDX-License-Identifier: GPL-3.0-or-later
#
# Partitioned convolution. A library rather than a module, because two things
# want it: the equaliser's linear-phase and minimum-phase modes, and -- when
# there is something to read an impulse response with -- a convolver proper.

add_library(mediaperch_convolve STATIC convolve.cpp)
target_include_directories(mediaperch_convolve PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_convolve
    PUBLIC mediaperch_transform PRIVATE mediaperch_flags)
set_target_properties(mediaperch_convolve PROPERTIES FOLDER "modules")

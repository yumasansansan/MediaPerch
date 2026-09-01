# SPDX-License-Identifier: GPL-3.0-or-later
#
# Second-order sections, shared the way modules/mp4 is: the equaliser is a
# cascade a person chose and the loudness meter is a cascade ITU-R BS.1770
# chose, and neither should own the arithmetic.

add_library(mediaperch_biquad STATIC biquad.cpp)
target_include_directories(mediaperch_biquad PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_biquad PRIVATE mediaperch_flags)
set_target_properties(mediaperch_biquad PROPERTIES FOLDER "modules")

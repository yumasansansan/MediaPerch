# SPDX-License-Identifier: GPL-3.0-or-later
#
# The transforms. They began inside the resampler and moved here when the
# equaliser needed the same three, which is the same reason modules/mp4 and
# modules/biquad exist.

add_library(mediaperch_transform STATIC transform.cpp)
target_include_directories(mediaperch_transform PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_transform PRIVATE mediaperch_flags)
set_target_properties(mediaperch_transform PROPERTIES FOLDER "modules")

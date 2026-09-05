# SPDX-License-Identifier: GPL-3.0-or-later
#
# A module's line to the host's log, shared the way pcm_format and biquad are:
# twelve modules had their own copy and the buffers had drifted to three sizes.
# See module_log.hpp.

add_library(mediaperch_module_log STATIC module_log.cpp)
target_include_directories(mediaperch_module_log PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_module_log PRIVATE MediaPerch::abi mediaperch_flags)
set_target_properties(mediaperch_module_log PROPERTIES FOLDER "modules")

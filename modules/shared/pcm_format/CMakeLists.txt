# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a bit depth means, shared the way biquad and transform are: six modules
# had their own copy and two of them disagreed. See pcm_format.hpp.

add_library(mediaperch_pcm_format STATIC pcm_format.cpp)
target_include_directories(mediaperch_pcm_format PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_pcm_format PRIVATE MediaPerch::abi mediaperch_flags)
set_target_properties(mediaperch_pcm_format PROPERTIES FOLDER "modules")

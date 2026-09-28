# SPDX-License-Identifier: GPL-3.0-or-later
#
# Header only: the loudest sample of a block, which the gain, the equaliser,
# the convolver, the downmix and the loudness meter all report, measured by the
# one loop.

add_library(mediaperch_peak INTERFACE)
target_include_directories(mediaperch_peak INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

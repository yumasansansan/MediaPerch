# SPDX-License-Identifier: GPL-3.0-or-later
#
# Header only: the resampling kernels' names, numbers and HLSL, shared by the
# scaler stage and the presenter's chroma reconstruction so that the two
# compile one formula set. cmake/ShaderPrecision.cmake reads this directory
# too, because the shader text lives in it.

add_library(mediaperch_kernels INTERFACE)
target_include_directories(mediaperch_kernels INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

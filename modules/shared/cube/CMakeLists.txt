# SPDX-License-Identifier: GPL-3.0-or-later
#
# A Cube LUT, read without a GPU.
#
# **Here rather than inside `modules/vdsp/lut` for the reason `h264` is here.**
# A parser is where the bugs are, and a parser that needed a graphics device to
# run would be a parser nobody fuzzes and nobody runs in CI. Nothing in it knows
# what Direct3D is: it turns text into a size, a domain and a table, and the
# stage turns that into a texture.
#
# It is also the half a Metal or a Vulkan stage would take unchanged.

add_library(mediaperch_cube STATIC cube.cpp)
target_include_directories(mediaperch_cube PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_cube PRIVATE mediaperch_flags)
set_target_properties(mediaperch_cube PROPERTIES FOLDER "modules")

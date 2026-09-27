# SPDX-License-Identifier: GPL-3.0-or-later
#
# dr_wav's implementation, compiled once for the two readers that use it.
# Somebody else's code, so MEDIAPERCH_EXTERNAL -- see mediaperch_flags_everywhere
# in cmake/CompilerOptions.cmake for what that does, and does not do.

add_library(mediaperch_dr_wav STATIC dr_wav_impl.c)
# And dr_wav_file.hpp, which is ours: dr_wav reading a file module_file.hpp
# opened, so a reader that includes it links mediaperch_module_log as well.
target_include_directories(mediaperch_dr_wav PUBLIC "${CMAKE_SOURCE_DIR}/external/dr_libs"
                                                    "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(mediaperch_dr_wav PROPERTIES MEDIAPERCH_EXTERNAL TRUE FOLDER "external")

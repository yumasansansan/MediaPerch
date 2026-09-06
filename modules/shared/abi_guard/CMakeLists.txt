# SPDX-License-Identifier: GPL-3.0-or-later
#
# Header only, and linked into every module by `mediaperch_add_module` rather
# than named by each one: every module's vtable is `noexcept`, so every module
# needs the fence, including the ones nobody has written yet.

add_library(mediaperch_abi_guard INTERFACE)
target_include_directories(mediaperch_abi_guard INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

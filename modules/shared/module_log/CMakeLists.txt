# SPDX-License-Identifier: GPL-3.0-or-later
#
# A module's line to the host's log, shared the way pcm_format and biquad are:
# twelve modules had their own copy and the buffers had drifted to three sizes.
# See module_log.hpp.

add_library(mediaperch_module_log STATIC module_log.cpp)
# And the files a module opens by name (module_file.hpp), which every module
# that opens one reaches through this library too: the Windows functions on
# Windows and ISO C's everywhere else, chosen here rather than by an `#if` in
# each module -- see the header.
if(WIN32)
    target_sources(mediaperch_module_log PRIVATE module_file_windows.cpp)
else()
    target_sources(mediaperch_module_log PRIVATE module_file_iso.cpp)
endif()
target_include_directories(mediaperch_module_log PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_module_log PRIVATE MediaPerch::abi mediaperch_flags)
set_target_properties(mediaperch_module_log PROPERTIES FOLDER "modules")

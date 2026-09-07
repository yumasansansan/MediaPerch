# SPDX-License-Identifier: GPL-3.0-or-later
#
# H.264's bitstream, read without a decoder.
#
# **Moved out of codec_mft because none of it is Media Foundation's.** The file
# said so from the day it was written -- "a container question wearing a codec's
# clothes" -- and the sequence parameter set reader added later is the same
# again: it exists so a `probe` can decline 4:2:2, 4:4:4 and ten bits before a
# decoder is opened, and every H.264 decoder in this tree will need to decline
# exactly those.
#
# Nothing here touches Media Foundation, Direct3D or a file. It is arithmetic
# over bytes and is tested as such.

add_library(mediaperch_h264 STATIC h264.cpp)
target_include_directories(mediaperch_h264 PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_h264 PRIVATE MediaPerch::abi mediaperch_flags)
set_target_properties(mediaperch_h264 PROPERTIES FOLDER "modules")

# **The same file, for a module that cannot link the archive.** A module pinned
# to the release runtime -- codec_de265, because libde265 is built once as a
# release library and is C++ -- links a Debug build of this archive as a
# `RuntimeLibrary` and `_ITERATOR_DEBUG_LEVEL` mismatch, which is how the
# Debug CI job failed. The archive is right for everything else that links it
# (codec_mft, demux_mp4, the tests all follow the configuration), so the pinned
# module compiles the source itself, with its own flags, and there is one copy
# of the source rather than one per runtime.
add_library(mediaperch_h264_sources INTERFACE)
target_sources(mediaperch_h264_sources INTERFACE "${CMAKE_CURRENT_LIST_DIR}/h264.cpp")
target_include_directories(mediaperch_h264_sources INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

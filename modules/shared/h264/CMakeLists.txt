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

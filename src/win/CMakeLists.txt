# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Windows head: the message-free half of the engine. It owns the COM
# apartment, MMCSS, logging and LoadLibrary, and it is the only code in the tree
# allowed to include <windows.h> outside a module.

add_library(mediaperch_win STATIC
    mediaperch/platform.cpp
    mediaperch/platform.hpp
    mediaperch/verify.cpp
    mediaperch/verify.hpp
    mediaperch/win_headers.hpp
)
add_library(MediaPerch::win ALIAS mediaperch_win)

target_include_directories(mediaperch_win PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_win
    PUBLIC MediaPerch::core
    PRIVATE mediaperch_flags mediaperch_waveformat avrt ole32 bcrypt shell32)

set_target_properties(mediaperch_win PROPERTIES FOLDER "win")

# Milestone 1's entire user interface.
add_executable(mediaperch_probe mediaperch/main.cpp)
target_link_libraries(mediaperch_probe PRIVATE MediaPerch::win mediaperch_flags)
set_target_properties(mediaperch_probe PROPERTIES
    OUTPUT_NAME "mediaperch-probe"
    FOLDER "win")

# The probe is useless without modules beside it -- and the decode-quality test
# drives it against every codec in the tree, so every module it could choose has
# to be there before that test runs.
add_dependencies(mediaperch_probe
    mp_sink_wasapi mp_decode_native mp_decode_mf mp_decode_ffmpeg
    mp_decode_alac mp_decode_aac mp_decode_mp3)

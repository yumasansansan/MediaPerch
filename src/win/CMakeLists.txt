# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Windows head: the message-free half of the engine. It owns the COM
# apartment, MMCSS, logging and LoadLibrary, and it is the only code in the tree
# allowed to include <windows.h> outside a module.

add_library(mediaperch_win STATIC
    mediaperch/engine_host.cpp
    mediaperch/engine_host.hpp
    mediaperch/ipc_client.cpp
    mediaperch/ipc_client.hpp
    mediaperch/ipc_server.cpp
    mediaperch/ipc_server.hpp
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

# The `loudness` command is the ReplayGain stage's own meter, run over a whole
# file instead of over a block. Sharing the library is what makes the number the
# probe prints and the number the stage reports the same number.
if(TARGET mediaperch_loudness)
    target_link_libraries(mediaperch_probe PRIVATE mediaperch_loudness)
    target_compile_definitions(mediaperch_probe PRIVATE MEDIAPERCH_HAS_LOUDNESS=1)
else()
    target_compile_definitions(mediaperch_probe PRIVATE MEDIAPERCH_HAS_LOUDNESS=0)
endif()
set_target_properties(mediaperch_probe PROPERTIES
    OUTPUT_NAME "mediaperch-probe"
    FOLDER "win")

# The probe is useless without modules beside it -- and the decode-quality test
# drives it against every codec in the tree, so every module it could choose has
# to be there before that test runs.
add_dependencies(mediaperch_probe
    mp_sink_wasapi mp_decode_native mp_decode_mf mp_decode_ffmpeg
    mp_decode_alac mp_decode_aac mp_decode_mp3)

# The engine. This is the product: no window, no toolkit, and nothing in the
# process that a shell could take down with it.
add_executable(mediaperchd mediaperchd/main.cpp)
target_link_libraries(mediaperchd PRIVATE MediaPerch::win mediaperch_flags)
set_target_properties(mediaperchd PROPERTIES OUTPUT_NAME "mediaperchd" FOLDER "win")
add_dependencies(mediaperchd
    mp_sink_wasapi mp_decode_native mp_decode_mf mp_decode_ffmpeg
    mp_decode_alac mp_decode_aac mp_decode_mp3)

# The shell that is always there. It links the client half of the IPC and the
# protocol, and nothing else: no graph, no modules, no device.
add_executable(mediaperch_cli ../../shell/cli/main.cpp)
target_link_libraries(mediaperch_cli PRIVATE MediaPerch::win mediaperch_flags)
set_target_properties(mediaperch_cli PROPERTIES
    OUTPUT_NAME "mediaperch-cli"
    FOLDER "shell")

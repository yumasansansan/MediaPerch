# SPDX-License-Identifier: GPL-3.0-or-later
#
# One Media Foundation transform, and none of Media Foundation's pipeline.
#
# Nothing is vendored: `mfplat`, `mfuuid` and the transform interfaces are in
# the Windows SDK the build already requires. What this module deliberately does
# *not* link is `mfreadwrite`, which is where `IMFSourceReader` lives -- the
# pipeline plan.md §9.8 explains this module exists in order not to use.

if(NOT WIN32)
    # Media Foundation is the module. A Linux decoder is VA-API behind the same
    # MpVideoCodecVtbl, and a D3D12 one is a sibling here.
    return()
endif()

mediaperch_add_module(mp_codec_mft
    KIND codec
    SOURCES
        codec_mft.cpp
        avcc.cpp)

target_include_directories(mp_codec_mft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mp_codec_mft PRIVATE mfplat mfuuid ole32 d3d11)

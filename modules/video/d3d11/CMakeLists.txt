# SPDX-License-Identifier: GPL-3.0-or-later
#
# The first MpVideoVtbl.
#
# Nothing is vendored: Direct3D 11, DXGI and the shader compiler all come from
# the Windows SDK, which the build already requires. `d3dcompiler` is the one
# worth a word -- the colour shader is compiled at run time rather than baked
# in, because a shader that can be read next to the comment explaining it is a
# shader somebody can check, and `d3dcompiler_47.dll` has shipped in Windows
# since 10.

if(NOT WIN32)
    # Direct3D is the whole module. The Linux head brings a different presenter
    # behind the same vtable, which is the point of the vtable.
    return()
endif()

mediaperch_add_module(mp_video_d3d11
    KIND video
    SOURCES
        video_d3d11.cpp
        colour_plan.cpp)

target_include_directories(mp_video_d3d11 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mp_video_d3d11 PRIVATE d3d11 dxgi d3dcompiler)

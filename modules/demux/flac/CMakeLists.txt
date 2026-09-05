# SPDX-License-Identifier: GPL-3.0-or-later
#
# The native FLAC container, on libFLAC -- which is asked where frames begin and
# end and never to decode one. `skip_single_frame` and `get_decode_position` are
# documented by Xiph for exactly this, and the module says why the first version
# of this file did not use them.
#
# It links the same target `codec_flac` does; the submodule is brought in there,
# and this directory is added first, so the guard below is what makes the order
# not matter.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/flac/CMakeLists.txt")
    message(WARNING
        "external/flac is empty -- run `git submodule update --init`. "
        "Skipping demux_flac; the tree still builds without it.")
    return()
endif()

mediaperch_add_module(mp_demux_flac
    KIND demux
    SOURCES demux_flac.cpp
    LINK FLAC)

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_flac PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_flac PRIVATE mediaperch_module_log)

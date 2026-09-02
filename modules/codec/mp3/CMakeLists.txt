# SPDX-License-Identifier: GPL-3.0-or-later
#
# MPEG audio layers I to III through dr_mp3's low-level entry point, which takes
# a frame and gives back its samples. `decode_mp3` used the high-level one, which
# opens files -- the same library, the other half of it.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/dr_libs/dr_mp3.h")
    message(WARNING
        "external/dr_libs is empty -- run `git submodule update --init`. "
        "Skipping codec_mp3.")
    return()
endif()

mediaperch_add_module(mp_codec_mp3
    KIND codec
    SOURCES codec_mp3.cpp dr_mp3_impl.c)

target_include_directories(mp_codec_mp3 PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

# Without this dr_mp3 decodes to int16 and converts up on the way out, which
# would be a quantisation performed inside a decoder. It is set on the target so
# the implementation unit and the module agree about the size of drmp3's internal
# frame buffer -- disagreeing there is a silent overrun.
target_compile_definitions(mp_codec_mp3 PRIVATE DR_MP3_FLOAT_OUTPUT)

# Somebody else's parser, held to somebody else's warning standard.
if(MSVC)
    set_source_files_properties(dr_mp3_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_mp3_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

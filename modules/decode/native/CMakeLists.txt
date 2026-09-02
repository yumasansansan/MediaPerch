# SPDX-License-Identifier: GPL-3.0-or-later
#
# WAV and FLAC from two public-domain headers. Like every module it links the ABI
# header and nothing else of ours.

mediaperch_add_module(mp_decode_native
    KIND decode
    SOURCES decode_native.cpp dr_libs_impl.c)

# dr_wav refuses any file above 384 kHz. That is its own sanity ceiling against
# garbage headers, not a WAV limit -- the field is 32 bits wide -- and it turned
# a perfectly good 768 kHz file into "unsupported by this module", which reads
# like our decision rather than a vendored constant. Raised to 128x48k, which is
# past every rate that exists (768 kHz PCM, 2.8 MHz for DSD1024 over DoP) while
# still bounding what a malformed header can claim.
target_compile_definitions(mp_decode_native PRIVATE DRWAV_MAX_SAMPLE_RATE=6144000)
target_include_directories(mp_decode_native PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

# The dr_libs translation unit is somebody else's code and is held to somebody
# else's warning standard. decode_native.cpp keeps ours.
if(MSVC)
    set_source_files_properties(dr_libs_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_libs_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()


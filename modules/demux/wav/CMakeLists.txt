# SPDX-License-Identifier: GPL-3.0-or-later
#
# WAV and its four relatives as a container, on dr_wav. Header-only, and paired
# with codec_pcm, which has no dependency at all -- so an install that builds no
# submodule with a build system still reads uncompressed audio.

mediaperch_add_module(mp_demux_wav
    KIND demux
    SOURCES demux_wav.cpp dr_wav_impl.c)

# dr_wav refuses any file above 384 kHz. That is its own sanity ceiling against
# garbage headers, not a WAV limit -- the field is 32 bits wide -- and it turned
# a perfectly good 768 kHz file into "unsupported by this module", which reads
# like our decision rather than a vendored constant. Raised to 128x48k, which is
# past every rate that exists (768 kHz PCM, 2.8 MHz for DSD1024 over DoP) while
# still bounding what a malformed header can claim.
target_compile_definitions(mp_demux_wav PRIVATE DRWAV_MAX_SAMPLE_RATE=6144000)
target_include_directories(mp_demux_wav PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

# The dr_wav translation unit is somebody else's code and is held to somebody
# else's warning standard. demux_wav.cpp keeps ours.
if(MSVC)
    set_source_files_properties(dr_wav_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(dr_wav_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_wav PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_wav PRIVATE mediaperch_module_log)

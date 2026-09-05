# SPDX-License-Identifier: GPL-3.0-or-later
#
# WAV and its four relatives as a container, on dr_wav. Header-only, and paired
# with codec_pcm, which has no dependency at all -- so an install that builds no
# submodule with a build system still reads uncompressed audio.

mediaperch_add_module(mp_demux_wav
    KIND demux
    SOURCES demux_wav.cpp
    # dr_wav's implementation, compiled once in modules/shared/dr_wav and held to
    # nobody's warning standard but its own. demux_wav.cpp keeps ours.
    LINK mediaperch_dr_wav)

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_wav PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_wav PRIVATE mediaperch_module_log)

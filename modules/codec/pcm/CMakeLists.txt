# SPDX-License-Identifier: GPL-3.0-or-later
#
# PCM, and nothing else at all: no submodule, no header-only library, no
# operating system codec. The smallest module in the tree, and the one every
# container that carries uncompressed audio now shares.

mediaperch_add_module(mp_codec_pcm
    KIND codec
    SOURCES codec_pcm.cpp)

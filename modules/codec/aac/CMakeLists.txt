# SPDX-License-Identifier: GPL-3.0-or-later
#
# AAC-LC as a codec and nothing else. `aac.cpp` was already the codec alone --
# 1,599 lines of it with no file handle in sight -- so this module is the vtable
# around it and the channel permutation, and nothing more.

mediaperch_add_module(mp_codec_aac
    KIND codec
    SOURCES codec_aac.cpp
    LINK mediaperch_aac)

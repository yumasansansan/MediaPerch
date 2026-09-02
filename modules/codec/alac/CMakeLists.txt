# SPDX-License-Identifier: GPL-3.0-or-later
#
# ALAC as a codec and nothing else. The library it links is unchanged: `alac.cpp`
# was always a pure codec that took a config and packets, which is why this
# module is a wrapper rather than a rewrite.

mediaperch_add_module(mp_codec_alac
    KIND codec
    SOURCES codec_alac.cpp
    LINK mediaperch_alac)

# SPDX-License-Identifier: GPL-3.0-or-later
#
# ALAC, written here: the bitstream in `alac.cpp` and the vtable around it.
#
# The library is separate from the module so that `alac_fuzzer` can link it
# directly. A parser reachable only through a module is a parser that cannot be
# fuzzed, and this one is the argument for having written our own ALAC at all --
# Apple's reference has been unmaintained since 2011, and ALHACK is what
# unmaintained decoders eventually produce.

add_library(mediaperch_alac STATIC alac.cpp)
target_include_directories(mediaperch_alac PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_alac PRIVATE mediaperch_flags)
set_target_properties(mediaperch_alac PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_codec_alac
    KIND codec
    SOURCES codec_alac.cpp
    LINK mediaperch_alac)

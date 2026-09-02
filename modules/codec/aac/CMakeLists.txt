# SPDX-License-Identifier: GPL-3.0-or-later
#
# AAC-LC, written here: 1,599 lines of bitstream with no file handle in sight,
# and the vtable around it.
#
# Separate library for the same reason ALAC's is: `aac_fuzzer` links it directly.
# Four maintained libraries were measured before this was written and each
# produced the wrong thing rather than a wrong sound -- see docs/formats.md.

add_library(mediaperch_aac STATIC aac.cpp aac_tables.cpp)
target_include_directories(mediaperch_aac PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_aac PRIVATE mediaperch_flags)
set_target_properties(mediaperch_aac PROPERTIES FOLDER "modules")

mediaperch_add_module(mp_codec_aac
    KIND codec
    SOURCES codec_aac.cpp
    LINK mediaperch_aac)

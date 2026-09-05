# SPDX-License-Identifier: GPL-3.0-or-later
#
# One function, shared by three decoders, because the mistake it corrects was
# made three times -- see decoder_threads.hpp.

add_library(mediaperch_decoder_threads INTERFACE)
target_include_directories(mediaperch_decoder_threads INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

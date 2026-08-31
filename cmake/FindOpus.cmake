# SPDX-License-Identifier: GPL-3.0-or-later
#
# The same trick as FindOgg.cmake, for opusfile's `find_package(Opus REQUIRED)`.
# See that file for why this works.
if(TARGET Opus::opus)
    set(Opus_FOUND TRUE)
    set(OPUS_FOUND TRUE)
    set(OPUS_LIBRARIES Opus::opus)
    set(OPUS_INCLUDE_DIRS "")
    return()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Opus
    REQUIRED_VARS _mediaperch_opus_target_missing
    FAIL_MESSAGE
        "MediaPerch builds libopus from external/opus. The Opus::opus target does "
        "not exist yet -- add external/opus before opusfile.")

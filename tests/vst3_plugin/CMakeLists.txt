# SPDX-License-Identifier: GPL-3.0-or-later
#
# A VST3, built by the tests, so the host has something to load.
#
# It is a `MODULE` and not a `SHARED` because that is what a plugin is, and the
# `.vst3` suffix because that is what the host looks for -- though this one is a
# plain DLL rather than a bundle, which is the older of the two shapes and the
# one that keeps a test from needing a directory tree.
#
# The SDK's own sources come from the one target that compiles them -- see
# modules/dsp/vst3 -- and bring the SDK's include path and definitions with
# them. What is compiled here is this tree's code, with this tree's warnings.

add_library(mediaperch_test_effect MODULE
    test_effect.cpp
    "${CMAKE_SOURCE_DIR}/modules/dsp/vst3/vst3_iids.cpp")

target_link_libraries(mediaperch_test_effect PRIVATE mediaperch_flags mediaperch_vst3_sdk)

set_target_properties(mediaperch_test_effect PROPERTIES
    PREFIX ""
    SUFFIX ".vst3"
    FOLDER "tests")

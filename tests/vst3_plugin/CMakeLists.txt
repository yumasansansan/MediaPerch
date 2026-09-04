# SPDX-License-Identifier: GPL-3.0-or-later
#
# A VST3, built by the tests, so the host has something to load.
#
# It is a `MODULE` and not a `SHARED` because that is what a plugin is, and the
# `.vst3` suffix because that is what the host looks for -- though this one is a
# plain DLL rather than a bundle, which is the older of the two shapes and the
# one that keeps a test from needing a directory tree.

set(vst3 "${CMAKE_SOURCE_DIR}/external/vst3sdk")

add_library(mediaperch_test_effect MODULE
    test_effect.cpp
    "${CMAKE_SOURCE_DIR}/modules/dsp/vst3/vst3_iids.cpp"
    "${vst3}/pluginterfaces/base/funknown.cpp"
    "${vst3}/pluginterfaces/base/coreiids.cpp"
    "${vst3}/pluginterfaces/base/ustring.cpp"
    "${vst3}/pluginterfaces/base/conststringtable.cpp")

target_include_directories(mediaperch_test_effect SYSTEM PRIVATE "${vst3}")
target_link_libraries(mediaperch_test_effect PRIVATE mediaperch_flags ole32)
target_compile_definitions(mediaperch_test_effect PRIVATE
    "$<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>")

set_target_properties(mediaperch_test_effect PROPERTIES
    PREFIX ""
    SUFFIX ".vst3"
    FOLDER "tests")

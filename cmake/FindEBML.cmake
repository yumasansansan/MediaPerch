# SPDX-License-Identifier: GPL-3.0-or-later
#
# Not a search. libebml is a submodule that this build already compiled, and
# this file exists only so that libmatroska -- which we did not write -- can go
# on calling `find_package(EBML 2.0.0)` and get the in-tree target.
#
# The same mechanism as cmake/FindOgg.cmake: `cmake/` is on CMAKE_MODULE_PATH
# from the top-level CMakeLists, so this is found before libmatroska's own
# search for an *installed* libebml. It is also why `modules/demux/mkv` adds
# external/libebml before external/libmatroska: the target has to exist for this
# file to hand it over.
#
# libebml builds a target called `ebml` and no namespaced alias, and libmatroska
# links `EBML::ebml`. Making that alias is the whole of what this does.

if(TARGET ebml)
    if(NOT TARGET EBML::ebml)
        add_library(EBML::ebml ALIAS ebml)
    endif()
    set(EBML_FOUND TRUE)
    set(EBML_VERSION "2.0.0")
    set(EBML_LIBRARIES EBML::ebml)
    set(EBML_INCLUDE_DIRS "")
    return()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EBML
    REQUIRED_VARS _mediaperch_ebml_target_missing
    FAIL_MESSAGE
        "MediaPerch builds libebml from external/libebml. The `ebml` target does "
        "not exist yet, which means external/libebml was added after the library "
        "that wants it -- add it first.")

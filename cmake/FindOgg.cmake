# SPDX-License-Identifier: GPL-3.0-or-later
#
# Not a search. libogg is a submodule that this build already compiled, and this
# file exists only so that libvorbis and opusfile -- neither of which we wrote --
# can go on calling `find_package(Ogg REQUIRED)` and get the in-tree target.
#
# Both of them ship a real FindOgg.cmake that looks for an *installed* libogg,
# and both reach it with `list(APPEND CMAKE_MODULE_PATH ...)`. Ours is already on
# that path from the top-level CMakeLists, so ours is found first. That ordering
# is the whole mechanism, and it is why this file is in cmake/ and not somewhere
# more obviously related to Ogg.
if(TARGET Ogg::ogg)
    set(Ogg_FOUND TRUE)
    set(OGG_FOUND TRUE)
    # vorbis/lib/CMakeLists.txt still uses the old spelling in one branch.
    set(OGG_LIBRARIES Ogg::ogg)
    set(OGG_INCLUDE_DIRS "")
    return()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ogg
    REQUIRED_VARS _mediaperch_ogg_target_missing
    FAIL_MESSAGE
        "MediaPerch builds libogg from external/ogg. The Ogg::ogg target does not "
        "exist yet, which means external/ogg was added after the library that "
        "wants it -- add it first.")

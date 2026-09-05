# SPDX-License-Identifier: GPL-3.0-or-later
#
# libebml calling `find_package(utf8cpp)` for an *installed* utfcpp -- and,
# when there is none, downloading one.
#
# That second half is why this file exists. libebml's CMakeLists falls back to
# FetchContent: a `git clone` of nemtrif/utfcpp from GitHub, at configure time,
# into the build tree. It worked, which is why nobody noticed; the only visible
# signs were a "Could not find a package configuration file" warning on every
# configure and CI paying for a clone on every run. docs/plan.md says nothing
# is downloaded at configure time except Catch2, and this is what makes that
# true again: utfcpp is a submodule pinned at the tag libebml would have
# fetched, and this module answers find_package with it.
#
# utfcpp is header-only and BSL-1.0. Its CMake defines an INTERFACE target
# `utf8cpp` and an alias `utf8::cpp`; libebml looks for `utf8cpp::utf8cpp`
# first and falls back to plain `utf8cpp`, so the alias below serves the first
# branch and anyone else who asks by the conventional name.

set(_utfcpp_root "${CMAKE_SOURCE_DIR}/external/utfcpp")
if(NOT EXISTS "${_utfcpp_root}/CMakeLists.txt")
    message(WARNING
        "external/utfcpp is missing -- run `git submodule update --init`. "
        "libebml will download utfcpp at configure time instead.")
    set(utf8cpp_FOUND FALSE)
    return()
endif()

if(NOT TARGET utf8cpp)
    add_subdirectory("${_utfcpp_root}" "${CMAKE_BINARY_DIR}/external/utfcpp" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(NOT TARGET utf8cpp::utf8cpp)
    add_library(utf8cpp::utf8cpp ALIAS utf8cpp)
endif()

set(utf8cpp_FOUND TRUE)
set(utf8cpp_VERSION 3.2.5)

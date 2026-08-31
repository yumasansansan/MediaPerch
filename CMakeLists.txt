# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(MediaPerch
    VERSION 0.1.0
    DESCRIPTION "A bit-exact audio and video player for WASAPI exclusive mode"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# A preset with "msvc" in its name that quietly configures with whatever `cc` is
# on PATH is a trap, and it is not a hypothetical one: the Ninja generator does
# not go looking for Visual Studio the way the VS generator does, so outside a
# developer prompt it finds MinGW GCC on a GitHub runner and Strawberry Perl's
# gcc on a typical developer machine -- and then builds cleanly, with nothing in
# the log saying the toolchain was wrong.
#
# Presets that mean a particular compiler say so, and this checks them.
set(MEDIAPERCH_EXPECT_TOOLSET "" CACHE STRING
    "Fail configuration unless CMAKE_CXX_COMPILER_ID matches this (MSVC, Clang, GNU)")

if(MEDIAPERCH_EXPECT_TOOLSET AND NOT CMAKE_CXX_COMPILER_ID STREQUAL MEDIAPERCH_EXPECT_TOOLSET)
    message(FATAL_ERROR
        "This preset expects ${MEDIAPERCH_EXPECT_TOOLSET} and got "
        "${CMAKE_CXX_COMPILER_ID} at ${CMAKE_CXX_COMPILER}.\n"
        "On Windows the Ninja generator only finds MSVC inside a developer "
        "prompt: run VC/Auxiliary/Build/vcvars64.bat first, or use the `vs` "
        "preset, which locates the toolset by itself.\n"
        "To build with this compiler on purpose, configure with "
        "-D MEDIAPERCH_EXPECT_TOOLSET=.")
endif()

option(MEDIAPERCH_BUILD_TESTS "Build the test suite" ON)
option(MEDIAPERCH_BUILD_FUZZERS "Build the libFuzzer targets (Clang only)" OFF)
option(MEDIAPERCH_BUILD_PLATFORM "Build the platform head and the modules" ON)

# There is no /std:c++23 in MSVC and no /std:c23 either; both are spelled
# "latest". CMake knows this, so CXX_STANDARD 23 is the portable way to ask.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(CompilerOptions)

# The module ABI. Header only, pure C, and the one thing every layer may see.
add_library(mediaperch_abi INTERFACE)
add_library(MediaPerch::abi ALIAS mediaperch_abi)
target_include_directories(mediaperch_abi INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")

add_subdirectory(src/core)

# Portable modules build wherever the core does.
if(MEDIAPERCH_BUILD_PLATFORM)
    add_subdirectory(modules/decode_native)
    add_subdirectory(modules/decode_flac)
    add_subdirectory(modules/decode_ogg)
endif()

if(MEDIAPERCH_BUILD_PLATFORM AND WIN32)
    add_subdirectory(modules/decode_ffmpeg)
    add_subdirectory(modules/decode_mf)
    add_subdirectory(modules/sink_wasapi)
    add_subdirectory(src/win)
endif()

if(MEDIAPERCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MEDIAPERCH_BUILD_FUZZERS)
    enable_testing()
    add_subdirectory(fuzz)
endif()

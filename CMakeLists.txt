# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(MediaPerch
    VERSION 0.1.0
    DESCRIPTION "A bit-exact audio and video player for WASAPI exclusive mode"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

option(MEDIAPERCH_BUILD_TESTS "Build the test suite" ON)
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

if(MEDIAPERCH_BUILD_PLATFORM AND WIN32
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/win/CMakeLists.txt")
    add_subdirectory(src/win)
endif()

if(MEDIAPERCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

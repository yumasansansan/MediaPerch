# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ABI, crossed from C.
#
# Off by default and not in CI, which is what plan.md §2 asked for -- it says
# "deleted afterwards" and this is the one place the plan is not followed to the
# letter. Deleting it would delete the evidence, and an off-by-default target
# costs the same as a deleted one until somebody turns it on:
#
#   cmake --preset ninja-msvc -DMEDIAPERCH_BUILD_ABI_PROBES=ON
#   cmake --build --preset ninja-msvc-release
#   mediaperch-probe --dsp list        # it is there
#   mediaperch-probe play --file X --dsp probe_c --verbose
#
# Compiled as C, deliberately and with no fallback to C++. A header that has
# only ever been compiled by a C++ compiler has not been shown to be a C header.

add_library(mp_probe_c MODULE probe_c.c)
set_target_properties(mp_probe_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
# MediaPerch::abi is the include path and nothing else, which is the point: a
# module needs one header and no library.
target_link_libraries(mp_probe_c PRIVATE MediaPerch::abi)

set_target_properties(mp_probe_c PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_probe_c"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "abi")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_probe_c PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# The graph, the negotiation and the ring, with no device and no compositor.

include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(Catch2)

add_executable(mediaperch_tests
    abi_test.cpp
    # Compiled as C on purpose. The ABI header's whole job is to be readable by a
    # toolchain we do not control, and a header only ever compiled as C++ has not
    # been tested for that.
    abi_header_c.c
    format_test.cpp
    graph_test.cpp
    negotiation_test.cpp
    property_test.cpp
    repack_test.cpp
    ring_test.cpp
    sine_test.cpp
)

# decode_alac's parsers are ours, so they get unit tests like the rest of the
# tree. They live in a module rather than in the core because they are not
# portable-by-policy code -- but the test suite does not care where a thing
# lives, only whether it is ours.
if(TARGET mediaperch_alac)
    target_sources(mediaperch_tests PRIVATE alac_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_alac mediaperch_mp4)
endif()

target_link_libraries(mediaperch_tests
    PRIVATE MediaPerch::core Catch2::Catch2WithMain mediaperch_flags)

set_target_properties(mediaperch_tests PROPERTIES FOLDER "tests")

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(mediaperch_tests)

# Not a unit test: a rule about the tree, checked the same way and at the same
# time as everything else.
add_test(NAME core_purity
    COMMAND "${CMAKE_COMMAND}"
            -D "MEDIAPERCH_CORE_DIR=${CMAKE_SOURCE_DIR}/src/core"
            -P "${CMAKE_SOURCE_DIR}/cmake/CorePurity.cmake")

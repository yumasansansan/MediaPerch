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
    compare_test.cpp
    convert_test.cpp
    dsp_test.cpp
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
    target_sources(mediaperch_tests PRIVATE alac_test.cpp aac_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_alac mediaperch_mp4 mediaperch_aac)
endif()

# The resampler is measured, not sampled: the tests take its response apart with
# a DFT and compare it with the specification it was designed to.
if(TARGET mediaperch_resample)
    target_sources(mediaperch_tests PRIVATE resample_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_resample)
endif()

# A downmix is a decision, so the test reads the coefficients rather than
# listening to what came out.
if(TARGET mediaperch_mix)
    target_sources(mediaperch_tests PRIVATE mix_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_mix)
endif()

# An equaliser's curve is computable, so the test computes it rather than
# listening for it -- and then runs a tone through to check the two agree.
# A convolution has a definition, so the test computes it the slow way and
# subtracts.
if(TARGET mediaperch_convolve)
    target_sources(mediaperch_tests PRIVATE convolve_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_convolve)
endif()

if(TARGET mediaperch_biquad)
    target_sources(mediaperch_tests PRIVATE eq_test.cpp autoeq_test.cpp
                   ../modules/dsp_eq/autoeq.cpp)
    target_include_directories(mediaperch_tests PRIVATE ../modules/dsp_eq)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_biquad)
endif()

# A loudness meter has a published calibration point, so the test uses it: EBU
# Tech 3341's conformance signals, and the numbers they are supposed to give.
if(TARGET mediaperch_loudness)
    target_sources(mediaperch_tests PRIVATE loudness_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_loudness)
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

# Also not a unit test: the decoders held against the audio that was encoded,
# which needs FFmpeg to build the corpus and so cannot be a Catch2 case. It
# skips itself where FFmpeg is absent; CI passes MEDIAPERCH_REQUIRE_FFMPEG so a
# check that silently does not run is a failure there rather than a shrug.
if(TARGET mediaperch_probe)
    add_test(NAME decode_quality
        COMMAND "${CMAKE_COMMAND}"
                -D "MEDIAPERCH_PROBE=$<TARGET_FILE:mediaperch_probe>"
                -D "MEDIAPERCH_WORK=${CMAKE_CURRENT_BINARY_DIR}/quality"
                -D "MEDIAPERCH_REQUIRE_FFMPEG=${MEDIAPERCH_REQUIRE_FFMPEG}"
                -P "${CMAKE_SOURCE_DIR}/cmake/DecodeQuality.cmake")
    # It builds a dozen files with FFmpeg and decodes each of them three times,
    # which is a couple of minutes -- so it carries a label, and `ctest -LE
    # quality` is the way to leave it out of a quick local run.
    set_tests_properties(decode_quality PROPERTIES TIMEOUT 900 LABELS quality)
endif()

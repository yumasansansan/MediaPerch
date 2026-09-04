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
    packet_test.cpp
    player_test.cpp
    property_test.cpp
    protocol_test.cpp
    repack_test.cpp
    ring_test.cpp
    settings_test.cpp
    sine_test.cpp
    transport_test.cpp
)

# `sink_asio`'s DoP unpack is in a header with no Windows in it, so the one part
# of an ASIO sink that can be checked without a driver is checked here. See
# dop_test.cpp for what would be wrong if it were wrong.
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/sink/asio/dop_unpack.hpp")
    target_sources(mediaperch_tests PRIVATE dop_test.cpp)
    target_include_directories(mediaperch_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/modules/sink/asio")
endif()

# The ALAC and AAC decoders and the ADTS framer are ours and are Rust, so their
# unit tests are in their crates: `cargo test` over the workspace, registered
# here as rust_modules because this directory is added after enable_testing()
# and an add_test before it is dropped silently.
mediaperch_add_rust_tests()

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

# An impulse response is a file, so the test writes one and reads it back.
if(TARGET mediaperch_impulse)
    target_sources(mediaperch_tests PRIVATE impulse_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_impulse)
endif()

if(TARGET mediaperch_biquad)
    target_sources(mediaperch_tests PRIVATE eq_test.cpp autoeq_test.cpp
                   ../modules/dsp/eq/autoeq.cpp)
    target_include_directories(mediaperch_tests PRIVATE ../modules/dsp/eq)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_biquad)
endif()

# A loudness meter has a published calibration point, so the test uses it: EBU
# Tech 3341's conformance signals, and the numbers they are supposed to give.
if(TARGET mediaperch_loudness)
    target_sources(mediaperch_tests PRIVATE loudness_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE mediaperch_loudness)
endif()

# The pipe, with a real pipe on the other side of it. Windows-only because the
# transport is: the protocol above it is in the core and is tested there, and
# the Linux head will bring its own socket and its own version of this file.
if(TARGET mediaperch_win)
    target_sources(mediaperch_tests PRIVATE ipc_test.cpp)
    target_link_libraries(mediaperch_tests PRIVATE MediaPerch::win)
endif()

# The VST3 host, with a VST3 this repository writes so that the test does not
# depend on what happens to be installed. `vst3_host.cpp` is compiled into the
# test binary the same way autoeq.cpp is above: a module's source is not a
# library, and the alternative is a static library that exists for one caller.
if(TARGET mp_dsp_vst3)
    add_subdirectory(vst3_plugin)
    target_sources(mediaperch_tests PRIVATE vst3_test.cpp
                   ../modules/dsp/vst3/vst3_host.cpp
                   ../modules/dsp/vst3/vst3_iids.cpp
                   ../external/vst3sdk/pluginterfaces/base/funknown.cpp
                   ../external/vst3sdk/pluginterfaces/base/coreiids.cpp
                   ../external/vst3sdk/pluginterfaces/base/ustring.cpp
                   ../external/vst3sdk/pluginterfaces/base/conststringtable.cpp)
    target_include_directories(mediaperch_tests PRIVATE ../modules/dsp/vst3)
    target_include_directories(mediaperch_tests SYSTEM PRIVATE ../external/vst3sdk)
    target_link_libraries(mediaperch_tests PRIVATE ole32)
    target_compile_definitions(mediaperch_tests PRIVATE
        "$<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>"
        # Passed in rather than searched for: the path differs between single-
        # and multi-config generators, and a test that went looking would be
        # testing the search.
        "MEDIAPERCH_TEST_VST3=\"$<TARGET_FILE:mediaperch_test_effect>\"")
    add_dependencies(mediaperch_tests mediaperch_test_effect)
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

# Also not a unit test: a second compiler front end over the portable sources.
# Both Clang presets in this tree turn the modules off, so most of the code has
# only ever been read by MSVC -- and that hid a constant that was quietly -1, a
# host pointer three modules kept and never read, and two loops that mixed
# signedness in a build that treats -Wconversion as a rule. Syntax-only, so it
# links nothing and takes about a second. See cmake/ClangSyntax.cmake.
add_test(NAME clang_syntax
    COMMAND "${CMAKE_COMMAND}"
            -D "MEDIAPERCH_ROOT=${CMAKE_SOURCE_DIR}"
            -D "MEDIAPERCH_BUILD_DIR=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/ClangSyntax.cmake")

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

    # The coverage matrix in the README, regenerated and compared. A table
    # nobody generates is a table that is wrong, so this one fails when the
    # README stops matching what the decoders here actually do.
    #
    # It stops at 24 bits, because that is where FFmpeg stops being trustworthy
    # as an encoder: docs/formats.md records that its FLAC encoder writes 24 bits
    # when asked for 32. The 32-bit rows are measured with reference encoders and
    # written down in that document instead -- a generated table that depended on
    # which tools a machine happened to have would mean something different on
    # every machine.
    add_test(NAME format_matrix
        COMMAND "${CMAKE_COMMAND}"
                -D "MEDIAPERCH_PROBE=$<TARGET_FILE:mediaperch_probe>"
                -D "MEDIAPERCH_WORK=${CMAKE_CURRENT_BINARY_DIR}/formats"
                -D "MEDIAPERCH_README=${CMAKE_SOURCE_DIR}/README.md"
                -D "MEDIAPERCH_REQUIRE_FFMPEG=${MEDIAPERCH_REQUIRE_FFMPEG}"
                -P "${CMAKE_SOURCE_DIR}/cmake/FormatMatrix.cmake")
    set_tests_properties(format_matrix PROPERTIES TIMEOUT 600 LABELS quality)
endif()

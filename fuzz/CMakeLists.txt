# SPDX-License-Identifier: GPL-3.0-or-later
#
# libFuzzer targets for every parser that reads a file somebody else wrote.
#
# Clang only: MSVC has no libFuzzer. That is not a limitation to work around --
# the project already builds on both compilers for other reasons, and fuzzing on
# one of them finds the same bugs.
#
#   cmake --preset fuzz
#   cmake --build --preset fuzz
#   build/fuzz/bin/wav_fuzzer corpus/wav -runs=100000

# libFuzzer's own runtime, clang_rt.fuzzer-x86_64.lib, is built against the
# static CRT. Objects here default to the dynamic one, and the two disagree
# through a /failifmismatch record that MSVC's STL headers emit -- so the
# mismatch only appears once a fuzz target includes <vector>, which is why it
# waited until alac_fuzzer to show up rather than failing on the first one.
# Matching libFuzzer is the fix; the alternative is fuzz targets that may not
# use the standard library.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "MEDIAPERCH_BUILD_FUZZERS needs Clang: libFuzzer does not exist for MSVC. "
        "Use the `fuzz` preset, or turn the option off.")
endif()

# ASan on Windows links its runtime dynamically, so the DLL has to sit beside the
# binary or the fuzzer exits before main with no message at all. Finding it is a
# one-line glob; not finding it costs an afternoon.
file(GLOB mediaperch_asan_runtime
     "$ENV{ProgramFiles}/LLVM/lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
if(mediaperch_asan_runtime)
    list(GET mediaperch_asan_runtime -1 mediaperch_asan_runtime) # the newest Clang
    message(STATUS "fuzzers: ASan runtime from ${mediaperch_asan_runtime}")
else()
    message(WARNING
        "Could not find clang_rt.asan_dynamic-x86_64.dll. The fuzzers will build "
        "and then exit silently before main.")
endif()

function(mediaperch_add_fuzzer name source corpus)
    # Extra translation units after the corpus name: the ALAC, AAC and FLAC
    # parsers are ours, so they are compiled into the fuzzer directly and get
    # instrumented with it. The dr_libs ones need no extra sources -- they are
    # headers.
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/external/dr_libs"
        "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/core"
        "${CMAKE_SOURCE_DIR}/modules/codec/alac"
        "${CMAKE_SOURCE_DIR}/modules/demux/mp4"
        "${CMAKE_SOURCE_DIR}/modules/codec/aac")

    # MSVC's STL annotates container bounds for ASan, and the annotations only
    # agree when every translation unit that touches a container was built the
    # same way. Ours are, but the runtime is not, and the mismatch shows up as a
    # container-overflow report inside std::vector rather than anywhere real.
    # Turning the annotations off costs a class of check the bounds checks in
    # alac.cpp already cover.
    target_compile_definitions(${name} PRIVATE
        _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)

    # Not mediaperch_flags: this is somebody else's parser compiled into our
    # binary, and holding it to our warning set would drown the signal.
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address -w)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address)

    set_target_properties(${name} PROPERTIES FOLDER "fuzz")

    if(mediaperch_asan_runtime)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${mediaperch_asan_runtime}" "$<TARGET_FILE_DIR:${name}>")
    endif()

    # A short run on the seed corpus, so CI notices a parser that has stopped
    # working even when nobody is running a long campaign.
    #
    # **The seeds are copied into a scratch directory rather than handed to the
    # fuzzer as one.** Naming scratch first was the earlier arrangement, on the
    # basis that libFuzzer merges into its first corpus argument -- and that is
    # true until a run reduces an input, which puts the smaller version back
    # where the original came from. The result is new files appearing inside
    # `fuzz/corpus/` that nobody chose to keep. Copying makes the seeds
    # unreachable for writing, which is what read-only inputs should be.
    #
    # An absolute corpus is one that lives somewhere else -- a submodule's,
    # where reusing it is the point rather than copying it. It is copied all the
    # same, for the same reason.
    if(IS_ABSOLUTE "${corpus}")
        set(seeds "${corpus}")
        get_filename_component(corpus_name "${corpus}" NAME)
    else()
        set(seeds "${CMAKE_CURRENT_LIST_DIR}/corpus/${corpus}")
        set(corpus_name "${corpus}")
    endif()
    set(scratch "${CMAKE_CURRENT_BINARY_DIR}/${name}_scratch")
    add_test(NAME fuzz_${name}
             COMMAND ${CMAKE_COMMAND}
                     -D "FUZZER=$<TARGET_FILE:${name}>"
                     -D "SEEDS=${seeds}"
                     -D "SCRATCH=${scratch}"
                     -D "ARGS=-runs=20000|-max_total_time=30|-print_final_stats=1"
                     -P "${CMAKE_CURRENT_LIST_DIR}/RunFuzzer.cmake")
endfunction()

mediaperch_add_fuzzer(wav_fuzzer wav_fuzzer.cpp wav)
mediaperch_add_fuzzer(flac_fuzzer flac_fuzzer.cpp flac)
mediaperch_add_fuzzer(mp3_fuzzer mp3_fuzzer.cpp mp3)
mediaperch_add_fuzzer(aac_fuzzer aac_fuzzer.cpp aac
                      "${CMAKE_SOURCE_DIR}/modules/codec/aac/aac.cpp"
                      "${CMAKE_SOURCE_DIR}/modules/codec/aac/aac_tables.cpp")
mediaperch_add_fuzzer(alac_fuzzer alac_fuzzer.cpp alac
                      "${CMAKE_SOURCE_DIR}/modules/codec/alac/alac.cpp")

# The container, which is Bento4 rather than a parser here since demux_mp4 was
# rewritten on it. Deleting the old target along with the old parser would have
# traded five hundred fuzzed lines for eighty thousand unfuzzed ones.
#
# **Bento4 is built again here, with the sanitizer on.** This build configures
# with MEDIAPERCH_BUILD_PLATFORM=OFF, so modules/demux/mp4 never runs and there
# is no `ap4` target to borrow -- and borrowing one would be the wrong thing
# anyway: a library linked in uninstrumented is a library the fuzzer runs
# through and reports nothing about. Every other fuzzer here compiles the parser
# it tests into its own binary for the same reason.
if(NOT TARGET ap4 AND EXISTS "${CMAKE_SOURCE_DIR}/external/Bento4/CMakeLists.txt")
    set(BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(MSVC_FORCE_STATIC_C_RUNTIME OFF CACHE BOOL "" FORCE)
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/Bento4"
                     "${CMAKE_BINARY_DIR}/external/Bento4" EXCLUDE_FROM_ALL)
endif()
if(TARGET ap4)
    # **`fuzzer-no-link` is the half that matters**: it is what puts coverage
    # counters in Bento4's code. With `address` alone the fuzzer still catches a
    # crash, but it is walking blind -- the first campaign here reported "36
    # inline 8-bit counters", which is this file's own harness and nothing else,
    # and every mutation it made was guesswork. `no-link` because the entry point
    # comes from the fuzzer target, not from the library.
    target_compile_options(ap4 PRIVATE -fsanitize=fuzzer-no-link,address -w)
    target_compile_definitions(ap4 PRIVATE
        _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    set_target_properties(ap4 PROPERTIES FOLDER "external")

    mediaperch_add_fuzzer(mp4_fuzzer mp4_fuzzer.cpp mp4)
    target_link_libraries(mp4_fuzzer PRIVATE ap4)
endif()


# The settings file, on DragonPerch's INI corpus rather than a second one of
# ours. The parser is shared as a submodule for the same reason: two INI parsers
# in one house would have two sets of edge cases and only one would be fuzzed.
mediaperch_add_fuzzer(settings_fuzzer settings_fuzzer.cpp
                      "${CMAKE_SOURCE_DIR}/external/dragonperch/fuzz/corpus/ini"
                      "${CMAKE_SOURCE_DIR}/src/core/mediaperch/settings.cpp"
                      "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core/dragonperch/ini.cpp")

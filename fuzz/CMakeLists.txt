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
    # Extra translation units after the corpus name: decode_alac's parsers are
    # ours, so they are compiled into the fuzzer directly and get instrumented
    # with it. The dr_libs ones need no extra sources -- they are headers.
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/external/dr_libs"
        "${CMAKE_SOURCE_DIR}/modules/decode_alac"
        "${CMAKE_SOURCE_DIR}/modules/mp4"
        "${CMAKE_SOURCE_DIR}/modules/decode_aac")

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
    # The scratch directory comes first on purpose: libFuzzer merges everything it
    # finds into its *first* corpus argument, so naming the tracked seed corpus
    # there turns a thirty-second CI run into three hundred new files in the
    # working tree. The seeds are read-only inputs; anything the run discovers
    # belongs in the build directory until somebody decides it is a regression.
    set(scratch "${CMAKE_CURRENT_BINARY_DIR}/${corpus}_scratch")
    file(MAKE_DIRECTORY "${scratch}")
    add_test(NAME fuzz_${name}
             COMMAND ${name} "${scratch}" "${CMAKE_CURRENT_LIST_DIR}/corpus/${corpus}"
                     -runs=20000 -max_total_time=30 -print_final_stats=1)
endfunction()

mediaperch_add_fuzzer(wav_fuzzer wav_fuzzer.cpp wav)
mediaperch_add_fuzzer(flac_fuzzer flac_fuzzer.cpp flac)
mediaperch_add_fuzzer(mp3_fuzzer mp3_fuzzer.cpp mp3)
mediaperch_add_fuzzer(aac_fuzzer aac_fuzzer.cpp aac
                      "${CMAKE_SOURCE_DIR}/modules/decode_aac/aac.cpp"
                      "${CMAKE_SOURCE_DIR}/modules/decode_aac/aac_tables.cpp")
mediaperch_add_fuzzer(alac_fuzzer alac_fuzzer.cpp alac
                      "${CMAKE_SOURCE_DIR}/modules/decode_alac/alac.cpp"
                      "${CMAKE_SOURCE_DIR}/modules/mp4/mp4.cpp")

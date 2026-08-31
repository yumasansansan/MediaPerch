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
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/external/dr_libs")

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

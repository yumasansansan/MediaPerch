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
# waited until a fuzz target included <vector> rather than failing on the first one.
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
    # Extra translation units after the corpus name: the AAC parser is ours,
    # so it is compiled into the fuzzer directly and gets instrumented with it.
    # The dr_libs ones need no extra sources -- they are headers.
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/external/dr_libs"
        "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/core"
        "${CMAKE_SOURCE_DIR}/modules/demux/mp4")

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
# MPEG audio is libmpg123 now, on both sides, so the fuzzer links it rather
# than compiling a parser of ours into itself. The library is built by
# modules/codec/mpa, which this build does not add -- so it is built here, with
# the sanitizer on, for the reason the Bento4 one gives: a library linked in
# uninstrumented is a library the fuzzer runs through and reports nothing about.
if(NOT TARGET libmpg123 AND EXISTS "${CMAKE_SOURCE_DIR}/external/mpg123/ports/cmake/CMakeLists.txt")
    set(BUILD_LIBOUT123 OFF CACHE BOOL "" FORCE)
    set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/mpg123/ports/cmake"
                     "${CMAKE_BINARY_DIR}/external/mpg123" EXCLUDE_FROM_ALL)
endif()
if(TARGET libmpg123)
    # The same INTERFACE path modules/codec/mpa adds: mpg123's CMake sets its
    # include paths per-directory, so the target carries none of its own.
    target_include_directories(libmpg123
        INTERFACE "${CMAKE_SOURCE_DIR}/external/mpg123/src/include")
    foreach(_ext IN ITEMS libmpg123 libsyn123 compat compat_str)
        if(TARGET ${_ext})
            # **`fuzzer-no-link` is the half that puts coverage counters in.**
            # With `address` alone libFuzzer runs the library blind: measured,
            # this target reported `cov: 20` -- the harness and nothing else --
            # over a quarter of a million executions. `no-link` because the
            # entry point comes from the fuzzer target, not from the library.
            target_compile_options(${_ext} PRIVATE -fsanitize=fuzzer-no-link,address -w)
            target_compile_definitions(${_ext} PRIVATE
                _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
            set_target_properties(${_ext} PROPERTIES FOLDER "external")
        endif()
    endforeach()
    mediaperch_add_fuzzer(mpa_fuzzer mpa_fuzzer.cpp mp3)
    target_link_libraries(mpa_fuzzer PRIVATE libmpg123)
endif()

# The Rust decoders' fuzzers -- ALAC, AAC and the ADTS framer: libfuzzer-sys
# carries libFuzzer's own sources and each target is in its crate's fuzz/
# directory, seeded by the same corpus the C++ fuzzer used, with the same input
# shape.
#
# **Coverage-guided, on stable.** cargo-fuzz wants nightly for its sanitizer
# flags, but the coverage half is LLVM's SanitizerCoverage pass, which stable
# rustc reaches through `-C passes=sancov-module`; libFuzzer supplies the
# `__sanitizer_cov_*` symbols those emit. AddressSanitizer stays out of reach,
# and for safe Rust that is the smaller loss -- the bounds checks are the
# sanitizer, and a missed one is a panic libFuzzer reports as a crash.
#
# Three things had to be found for that to link on MSVC, each measured as a
# link error first: `--target` is spelled out because RUSTFLAGS otherwise reach
# cargo's build scripts too (getrandom's failed to link, LNK1120); the entry
# point is named because libFuzzer's `main` lives inside an archive and
# link.exe will not infer an entry from one (LNK1561); and the coverage
# section's boundary symbols, which an ELF linker synthesises, come from a
# one-page C shim in modules/shared/fuzz on COFF (LNK2001,
# `__start___sancov_cntrs`). The PC table is left off: it wants the same shim's
# `__sancov_pcs` half and is only for -print_pcs, not for finding bugs.
function(mediaperch_add_rust_fuzzer name manifest corpus)
    set(rust_target_dir "${CMAKE_BINARY_DIR}/cargo")
    set(rust_fuzz_exe "${rust_target_dir}/${rust_host}/release/${name}${CMAKE_EXECUTABLE_SUFFIX}")
    # **One codegen unit, and it is not optional.** With several, the AAC
    # decoder's build died in LLVM with `Associative COMDAT symbol ... does not
    # exist`: the coverage pass attaches a counter section to each function's
    # COMDAT, and a generic instantiation that one unit references and another
    # unit later drops leaves a section pointing at nothing. Five flag
    # combinations were tried and this is the one that links; it costs a
    # slower build and nothing at run time.
    set(sancov
        "-Cpasses=sancov-module"
        "-Cllvm-args=-sanitizer-coverage-level=4"
        "-Cllvm-args=-sanitizer-coverage-inline-8bit-counters"
        "-Cllvm-args=-sanitizer-coverage-trace-compares"
        "-Ccodegen-units=1")
    if(MSVC OR WIN32)
        list(APPEND sancov "-Clink-arg=/ENTRY:mainCRTStartup")
    endif()
    string(JOIN " " sancov_flags ${sancov})
    add_custom_target(${name} ALL
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${rust_target_dir}"
                "RUSTFLAGS=${sancov_flags}"
                "${MEDIAPERCH_CARGO}" build --quiet --release --target "${rust_host}"
                --manifest-path "${manifest}"
        COMMENT "cargo: ${name} (libFuzzer, coverage on stable)"
        VERBATIM)
    set_target_properties(${name} PROPERTIES FOLDER "fuzz")
    add_test(NAME fuzz_${name}
             COMMAND ${CMAKE_COMMAND}
                     -D "FUZZER=${rust_fuzz_exe}"
                     -D "SEEDS=${corpus}"
                     -D "SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/${name}_scratch"
                     -D "ARGS=-runs=20000|-max_total_time=30|-print_final_stats=1"
                     -P "${CMAKE_CURRENT_LIST_DIR}/RunFuzzer.cmake")
endfunction()

if(MEDIAPERCH_CARGO)
    execute_process(COMMAND rustc -vV OUTPUT_VARIABLE rustc_info)
    string(REGEX MATCH "host: ([^
]+)" _ "${rustc_info}")
    set(rust_host "${CMAKE_MATCH_1}")
    mediaperch_add_rust_fuzzer(alac_fuzzer
        "${CMAKE_SOURCE_DIR}/modules/codec/alac/fuzz/Cargo.toml"
        "${CMAKE_CURRENT_LIST_DIR}/corpus/alac")
    mediaperch_add_rust_fuzzer(aac_fuzzer
        "${CMAKE_SOURCE_DIR}/modules/codec/aac/fuzz/Cargo.toml"
        "${CMAKE_CURRENT_LIST_DIR}/corpus/aac")
    mediaperch_add_rust_fuzzer(adts_fuzzer
        "${CMAKE_SOURCE_DIR}/modules/demux/adts/fuzz/Cargo.toml"
        "${CMAKE_CURRENT_LIST_DIR}/corpus/adts")
else()
    message(WARNING "cargo is not on PATH -- the Rust fuzzers are skipped")
endif()

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

# SPDX-License-Identifier: GPL-3.0-or-later
#
# libFuzzer targets for every parser that reads a file somebody else wrote.
#
# Clang, as the whole tree is: libFuzzer is LLVM's, and so is every compiler
# this project uses (cmake/LLVMToolchain.cmake).
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
#
# **In the resource directory of the Clang this build uses**, asked of that
# Clang. It used to be looked for under Program Files, which is where the LLVM
# installer puts one -- and where CI's runner image keeps an LLVM of its own,
# of whatever version, beside the pinned one ci/setup.sh unpacks elsewhere. A
# runtime from another version of the compiler is another ABI.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
    OUTPUT_VARIABLE mediaperch_clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE)
file(TO_CMAKE_PATH "${mediaperch_clang_resources}" mediaperch_clang_resources)
file(GLOB mediaperch_asan_runtime
     "${mediaperch_clang_resources}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
if(mediaperch_asan_runtime)
    list(GET mediaperch_asan_runtime 0 mediaperch_asan_runtime)
    message(STATUS "fuzzers: ASan runtime from ${mediaperch_asan_runtime}")
else()
    message(WARNING
        "Could not find clang_rt.asan_dynamic-x86_64.dll. The fuzzers will build "
        "and then exit silently before main.")
endif()

# **On Windows, a fuzzer that stops with STATUS_BREAKPOINT (0x80000003) inside
# a catch handler, with no report, has found LLVM rather than the code under
# it.** SanitizerCoverage puts its calls -- before an indirect call, before a
# comparison -- into catch and cleanup funclets without the "funclet" bundle a
# call there must carry, and WinEHPrepare deletes each such call and all that
# follows it in its block: the handler falls through into the int3 padding
# after the function. The Matroska fuzzer meets it wherever libebml's
# EbmlMaster::Read deletes an element in `catch (...)` before rethrowing
# (llvm/llvm-project#212404). It is left as a known false positive until LLVM
# carries the fix, not hidden: taking coverage out of the functions it hits
# would blind the fuzzer to them.

# What every fuzz test is run with. Thirty seconds, or twenty thousand inputs
# if those come first, on every push -- a smoke test that the campaign still
# builds and a place for the regression corpus to live; the nightly workflow
# runs the same tests for longer (RunFuzzer.cmake). **-timeout=25 makes a hang
# a finding**: without it one input that never ends holds the test until
# ctest gives up on it, twenty-five minutes later, and says only that it took
# too long.
set(mediaperch_fuzz_args "-runs=20000|-max_total_time=30|-timeout=25|-print_final_stats=1")
# Where the tests put what they find, for CI to keep: one directory, every
# file named after its fuzzer.
set(mediaperch_fuzz_findings "${CMAKE_BINARY_DIR}/fuzz-findings")

function(mediaperch_add_fuzzer name source corpus)
    # Extra translation units after the corpus name: the AAC parser is ours,
    # so it is compiled into the fuzzer directly and gets instrumented with it.
    # The dr_libs ones need no extra sources -- they are headers.
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/external/dr_libs"
        "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/engine"
        "${CMAKE_SOURCE_DIR}/src/player"
        "${CMAKE_SOURCE_DIR}/modules/demux/mp4")

    # MSVC's STL annotates container bounds for ASan, and the annotations only
    # agree when every translation unit that touches a container was built the
    # same way. Ours are, but the runtime is not, and the mismatch shows up as a
    # container-overflow report inside std::vector rather than anywhere real.
    # Turning the annotations off costs a class of check the bounds checks in
    # alac.cpp already cover.
    target_compile_definitions(${name} PRIVATE
        _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    # **The STL's own preconditions, checked in a release build**: an index
    # past a vector's end, front() of nothing, a span taken past its data. ASan
    # sees a read outside an allocation; these see a read outside the object,
    # inside an allocation that is bigger -- a vector's spare capacity, say --
    # which ASan cannot. A broken one traps, and libFuzzer keeps the input.
    target_compile_definitions(${name} PRIVATE _MSVC_STL_HARDENING=1)

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
                     -D "NAME=${name}"
                     -D "SEEDS=${seeds}"
                     -D "SCRATCH=${scratch}"
                     -D "FINDINGS=${mediaperch_fuzz_findings}"
                     -D "ARGS=${mediaperch_fuzz_args}"
                     -P "${CMAKE_CURRENT_LIST_DIR}/RunFuzzer.cmake")
endfunction()

# A library the fuzzers link, built with them rather than borrowed: a library
# linked in uninstrumented is a library the fuzzer runs through and reports
# nothing about. **`fuzzer-no-link` is the half that puts coverage counters
# in**: with `address` alone libFuzzer runs the library blind -- measured,
# libmpg123 reported `cov: 20`, the harness and nothing else, over a quarter of
# a million executions. `no-link` because the entry point comes from the fuzzer
# target, not from the library. Per language, because libwavpack carries
# assembly and an assembler handed a compiler's options fails; the annotations
# are off for the reason mediaperch_add_fuzzer gives.
function(mediaperch_instrument target)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=fuzzer-no-link,address>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-w>")
    target_compile_definitions(${target} PRIVATE
        _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION
        "$<$<COMPILE_LANGUAGE:CXX>:_MSVC_STL_HARDENING=1>")
    set_target_properties(${target} PROPERTIES FOLDER "external")
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
            mediaperch_instrument(${_ext})
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
    # The instruction set the modules are built for (cmake/Rust.cmake), in the
    # same RUSTFLAGS, since cargo reads only one. With `--target` spelled out
    # it reaches the fuzzers and not libfuzzer-sys's build script.
    if(MEDIAPERCH_RUSTFLAGS)
        list(APPEND sancov "${MEDIAPERCH_RUSTFLAGS}")
    endif()
    string(JOIN " " sancov_flags ${sancov})
    # **libFuzzer itself is C++, compiled by the cc crate, and it is given the
    # Clang this build was checked against.** libfuzzer-sys builds it from
    # source. The presets put `CC=clang` in the environment, which the cc crate
    # takes where it used to find cl.exe -- and Clang's GNU driver, unlike cl,
    # compiles for no C runtime unless it is told one, so the objects named
    # neither `msvcrt` nor `oldnames`, and libFuzzer's `putenv`, which only
    # oldnames maps to `_putenv`, did not link. The compilers are named here by
    # path rather than inherited, and the runtime is rustc's own: the dynamic
    # one.
    set(rust_cc_env "")
    if(WIN32)
        set(rust_cc_env
            "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
            "CFLAGS=-fms-runtime-lib=dll" "CXXFLAGS=-fms-runtime-lib=dll")
    endif()
    add_custom_target(${name} ALL
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${rust_target_dir}"
                "RUSTFLAGS=${sancov_flags}"
                ${rust_cc_env}
                ${MEDIAPERCH_CARGO_ENV}
                "${MEDIAPERCH_CARGO}" build --quiet --release --target "${rust_host}"
                --manifest-path "${manifest}"
        COMMENT "cargo: ${name} (libFuzzer, coverage on stable)"
        VERBATIM)
    set_target_properties(${name} PROPERTIES FOLDER "fuzz")
    add_test(NAME fuzz_${name}
             COMMAND ${CMAKE_COMMAND}
                     -D "FUZZER=${rust_fuzz_exe}"
                     -D "NAME=${name}"
                     -D "SEEDS=${corpus}"
                     -D "SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/${name}_scratch"
                     -D "FINDINGS=${mediaperch_fuzz_findings}"
                     -D "ARGS=${mediaperch_fuzz_args}"
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
    mediaperch_add_rust_fuzzer(dsd_fuzzer
        "${CMAKE_SOURCE_DIR}/modules/demux/dsd/fuzz/Cargo.toml"
        "${CMAKE_CURRENT_LIST_DIR}/corpus/dsd")
else()
    message(WARNING "cargo is not on PATH -- the Rust fuzzers are skipped")
endif()

# WavPack, which demux_wavpack is built on. It is on OSS-Fuzz upstream, which
# is why this is expected to find nothing -- and is not a reason to leave it
# out, for the reasons wavpack_fuzzer.cpp gives.
if(NOT TARGET wavpack AND EXISTS "${CMAKE_SOURCE_DIR}/external/wavpack/CMakeLists.txt")
    # No assembly under the sanitizer, and no GNU as found for it either: see
    # modules/demux/wavpack/CMakeLists.txt for what libwavpack does otherwise.
    set(CMAKE_ASM-ATT_COMPILER "")
    set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(WAVPACK_ENABLE_DSD ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/wavpack"
                     "${CMAKE_BINARY_DIR}/external/wavpack" EXCLUDE_FROM_ALL)
endif()
if(TARGET wavpack)
    mediaperch_instrument(wavpack)
    mediaperch_add_fuzzer(wavpack_fuzzer wavpack_fuzzer.cpp wavpack)
    target_link_libraries(wavpack_fuzzer PRIVATE wavpack)
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
    # Instrumented, as every library here is: Bento4's first campaign, before
    # it was, reported "36 inline 8-bit counters" -- this file's own harness
    # and nothing else -- and every mutation it made was guesswork.
    mediaperch_instrument(ap4)

    mediaperch_add_fuzzer(mp4_fuzzer mp4_fuzzer.cpp mp4)
    target_link_libraries(mp4_fuzzer PRIVATE ap4)
endif()


# The settings file, on DragonPerch's INI corpus rather than a second one of
# ours. The parser is shared as a submodule for the same reason: two INI parsers
# in one house would have two sets of edge cases and only one would be fuzzed.
# protocol.cpp comes along because settings.cpp describes its rows with the
# spec grammar parse_setting_spec reads, and that lives with the wire format.
mediaperch_add_fuzzer(settings_fuzzer settings_fuzzer.cpp
                      "${CMAKE_SOURCE_DIR}/external/dragonperch/fuzz/corpus/ini"
                      "${CMAKE_SOURCE_DIR}/src/player/mediaperch/settings.cpp"
                      "${CMAKE_SOURCE_DIR}/src/player/mediaperch/protocol.cpp"
                      "${CMAKE_SOURCE_DIR}/external/dragonperch/src/core/dragonperch/ini.cpp")

# ---------------------------------------------------------------------------
# Every demuxer, through the module ABI
# ---------------------------------------------------------------------------
#
# The fuzzers above hand a parser its bytes. What sits between such a parser
# and the host -- the module's own reading of the container, its index, its
# packets, its seek -- is fuzzed here: demux_fuzzer.cpp drives a demuxer the
# way the host does, with the module's own source and every library under it
# compiled in and instrumented. That is the code in which the format matrix,
# not a fuzzer, found demux_mkv casting whatever libebml had found to the
# header it had asked for. One fuzzer per module, because each defines
# mp_module_entry; the seeds are the parser fuzzer's where there is one.
#
# The module is compiled with what the platform build gives it, less the
# warnings that build holds it to already: the definitions mediaperch_flags
# gives a module on Windows, and the shared sources it links. The codec fuzzers
# further down are made the same way, with a harness of their own.
function(mediaperch_add_module_fuzzer name harness corpus module_source)
    cmake_parse_arguments(M "" "" "SOURCES;LINK" ${ARGN})
    mediaperch_add_fuzzer(${name} ${harness} "${corpus}" "${module_source}"
        "${CMAKE_SOURCE_DIR}/modules/shared/module_log/module_log.cpp"
        ${M_SOURCES})
    get_filename_component(module_dir "${module_source}" DIRECTORY)
    # tests/ for the driver and the scratch files, which the test that hands
    # every demuxer broken copies of real files shares with these.
    target_include_directories(${name} PRIVATE
        "${module_dir}"
        "${CMAKE_SOURCE_DIR}/modules/shared/abi_guard"
        "${CMAKE_SOURCE_DIR}/modules/shared/h264"
        "${CMAKE_SOURCE_DIR}/modules/shared/module_log"
        "${CMAKE_SOURCE_DIR}/modules/shared/pcm_format"
        "${CMAKE_SOURCE_DIR}/src/win"
        "${CMAKE_SOURCE_DIR}/tests")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    # **Undefined behaviour in this tree's own code is a crash here**: an
    # overflow of a signed number, a shift past the width, a misaligned load, a
    # pointer taken past its object. Trapping rather than reporting, so it needs
    # no runtime of its own beside ASan's, and only on the sources compiled into
    # this target -- the module's and the shared ones; the libraries under it
    # are other people's, whose undefined behaviour has its own fuzzers.
    target_compile_options(${name} PRIVATE -fsanitize=undefined -fsanitize-trap=undefined)
    target_link_libraries(${name} PRIVATE ${M_LINK})
endfunction()

function(mediaperch_add_demux_fuzzer module corpus)
    mediaperch_add_module_fuzzer(demux_${module}_fuzzer demux_fuzzer.cpp "${corpus}"
        "${CMAKE_SOURCE_DIR}/modules/demux/${module}/demux_${module}.cpp" ${ARGN})
endfunction()

set(mediaperch_pcm_format_source
    "${CMAKE_SOURCE_DIR}/modules/shared/pcm_format/pcm_format.cpp")
set(mediaperch_h264_source "${CMAKE_SOURCE_DIR}/modules/shared/h264/h264.cpp")

mediaperch_add_demux_fuzzer(wav wav
    SOURCES "${mediaperch_pcm_format_source}"
            "${CMAKE_SOURCE_DIR}/modules/shared/dr_wav/dr_wav_impl.c")
if(TARGET libmpg123)
    mediaperch_add_demux_fuzzer(mpa mp3 LINK libmpg123)
endif()
if(TARGET wavpack)
    mediaperch_add_demux_fuzzer(wavpack wavpack
        SOURCES "${mediaperch_pcm_format_source}" LINK wavpack)
endif()
if(TARGET ap4)
    mediaperch_add_demux_fuzzer(mp4 mp4 SOURCES "${mediaperch_h264_source}" LINK ap4)
endif()

# libFLAC, which demux_flac reads frames with, built here as modules/codec/flac
# builds it. FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION is its own switch for a
# fuzzer, as OSS-Fuzz uses it: a frame whose CRC does not match is decoded
# rather than dropped, so a mutation reaches the decoder instead of stopping at
# a checksum it has one chance in 65536 of getting right.
if(NOT TARGET FLAC AND EXISTS "${CMAKE_SOURCE_DIR}/external/flac/CMakeLists.txt")
    set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
    set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
    set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(WITH_OGG OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/flac"
                     "${CMAKE_BINARY_DIR}/external/flac" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(TARGET FLAC)
    mediaperch_instrument(FLAC)
    target_compile_definitions(FLAC PRIVATE FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
    mediaperch_add_demux_fuzzer(flac flac SOURCES "${mediaperch_pcm_format_source}" LINK FLAC)
    # The same switch fails allocations inside libFLAC when the fuzzer says to,
    # and the fuzzer owns the counters that say it: flac_allocations.hpp.
    target_compile_definitions(demux_flac_fuzzer PRIVATE MEDIAPERCH_FUZZ_FLAC_ALLOCATIONS)
endif()

# libogg, which demux_ogg pages with, built as modules/demux/ogg builds it.
# DISABLE_CRC is libogg's switch for a fuzzer, and for the same reason: a page
# whose checksum is wrong is skipped whole, and nearly every mutation makes it
# wrong. The seeds are a quarter of a second of a sine in each mapping the
# module names -- Vorbis, Opus, FLAC and Speex -- written by FFmpeg.
if(NOT TARGET ogg AND EXISTS "${CMAKE_SOURCE_DIR}/external/ogg/CMakeLists.txt")
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/ogg"
                     "${CMAKE_BINARY_DIR}/external/ogg" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(TARGET ogg)
    mediaperch_instrument(ogg)
    target_compile_definitions(ogg PRIVATE DISABLE_CRC)
    mediaperch_add_demux_fuzzer(ogg ogg LINK Ogg::ogg)
endif()

# libebml and libmatroska, which demux_mkv reads Matroska with, built as
# modules/demux/mkv builds them. The seeds are the Matroska and WebM files the
# tests read, audio and video both, from tests/data/mkv.
if(NOT TARGET matroska AND EXISTS "${CMAKE_SOURCE_DIR}/external/libmatroska/CMakeLists.txt"
   AND EXISTS "${CMAKE_SOURCE_DIR}/external/libebml/CMakeLists.txt")
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(DISABLE_PKGCONFIG ON CACHE BOOL "" FORCE)
    set(DISABLE_CMAKE_CONFIG ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/libebml"
                     "${CMAKE_BINARY_DIR}/external/libebml" EXCLUDE_FROM_ALL SYSTEM)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/libmatroska"
                     "${CMAKE_BINARY_DIR}/external/libmatroska" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(TARGET matroska AND TARGET ebml)
    mediaperch_instrument(ebml)
    mediaperch_instrument(matroska)
    mediaperch_add_demux_fuzzer(mkv "${CMAKE_SOURCE_DIR}/tests/data/mkv"
        SOURCES "${mediaperch_pcm_format_source}" "${mediaperch_h264_source}"
        LINK matroska ebml)
endif()

# ---------------------------------------------------------------------------
# Every audio codec, through the module ABI
# ---------------------------------------------------------------------------
#
# What a demuxer cuts out of a file -- the packets, and the configuration blob
# before them -- goes to a codec module verbatim, and none of the fuzzers above
# reaches one. codec_fuzzer.cpp drives a codec the way the host does, with the
# module's source and the library under it compiled in and instrumented. The
# seeds are the files in corpus/ taken apart into its input by
# tools/make_codec_seeds.py.
function(mediaperch_add_codec_fuzzer module)
    mediaperch_add_module_fuzzer(codec_${module}_fuzzer codec_fuzzer.cpp codec_${module}
        "${CMAKE_SOURCE_DIR}/modules/codec/${module}/codec_${module}.cpp" ${ARGN})
endfunction()

mediaperch_add_codec_fuzzer(pcm)
if(TARGET libmpg123)
    mediaperch_add_codec_fuzzer(mpa LINK libmpg123)
endif()
if(TARGET FLAC)
    mediaperch_add_codec_fuzzer(flac SOURCES "${mediaperch_pcm_format_source}" LINK FLAC)
    target_compile_definitions(codec_flac_fuzzer PRIVATE MEDIAPERCH_FUZZ_FLAC_ALLOCATIONS)
endif()

# libopus, built as modules/codec/opus builds it, with OPUS_ASSERTIONS -- its
# own switch for testing, as OSS-Fuzz builds it: the decoder's inner
# invariants, checked, so a packet that breaks one is a crash here rather than
# wrong audio.
if(NOT TARGET opus AND EXISTS "${CMAKE_SOURCE_DIR}/external/opus/CMakeLists.txt")
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_ASSERTIONS ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/opus"
                     "${CMAKE_BINARY_DIR}/external/opus" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(TARGET opus)
    mediaperch_instrument(opus)
    mediaperch_add_codec_fuzzer(opus LINK Opus::opus)
endif()

# libvorbis, built as modules/codec/vorbis builds it, on the libogg above: the
# codec reads its bits with libogg's bit packer.
if(NOT TARGET vorbis AND TARGET Ogg::ogg
   AND EXISTS "${CMAKE_SOURCE_DIR}/external/vorbis/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/external/vorbis"
                     "${CMAKE_BINARY_DIR}/external/vorbis" EXCLUDE_FROM_ALL SYSTEM)
endif()
if(TARGET vorbis)
    mediaperch_instrument(vorbis)
    mediaperch_add_codec_fuzzer(vorbis LINK vorbis Ogg::ogg)
endif()

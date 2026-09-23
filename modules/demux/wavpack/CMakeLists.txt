# SPDX-License-Identifier: GPL-3.0-or-later
#
# WavPack, on libwavpack -- and where the submodule enters the build.
#
# One module, not two: libwavpack has no block-level API to split a container
# from a codec with, so this names the codec its payload actually is --
# MP_CODEC_PCM, or MP_CODEC_DSD for a DSD file -- and hands packets to the
# module that already reads it. demux_wavpack.cpp has the argument.
#
# BSD-3-Clause, which a GPL-3.0 program may link. See docs/plan.md for the
# licence table.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/wavpack/CMakeLists.txt")
    message(WARNING
        "external/wavpack is missing -- run `git submodule update --init`. "
        "Skipping demux_wavpack; the tree still builds without it.")
    return()
endif()

# The command line tools are a whole second program, and the CMake package and
# pkg-config files are for an install this never does. Threads stay on: they are
# libwavpack's own worker pool for decoding, off by default at the API and asked
# for per file, so nothing starts a thread this module did not ask it to.
set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_DSD ON CACHE BOOL "" FORCE)

# **The assembly is on, and getting it reliably on took working around a bug
# upstream.**
#
# WavPack's CMakeLists calls `enable_language(ASM_MASM)` guarded by
# `WavPack_CPU_X64` -- and sets that variable ninety lines *later*, in
# `WavPack_DetectTargetCPUArchitectures`. On a first configure the guard is
# therefore false and the assembly is off; the detection then caches the
# variable, so the *second* configure sees it, enables MASM, and turns the
# assembly on. Measured: a fresh tree had `WAVPACK_ENABLE_ASM` empty and a tree
# configured twice had it `ON`, from the same source. That is a build whose
# output depends on how many times it has been configured.
#
# Enabling the language here, before the subdirectory is added, makes the guard
# true the first time. **There is no reproducibility cost to pay for the speed**:
# WavPack is lossless, so the two paths must agree or one is broken, and they do
# -- five files including a lossy hybrid one, identical bytes with and without.
# The gain is 13%: 60 seconds of 24-bit 96 kHz in 0.336 s rather than 0.387 s.
#
# **And the assembler is LLVM's.** The x64 files are MASM, which was ml64's to
# assemble while MSVC built this tree; they are llvm-ml's now, the
# MASM-compatible assembler that ships beside Clang, checked with the rest of
# the toolchain. Two things stand between the upstream files and it, and both
# are met here rather than patched into the submodule:
#
#   * `include <ksamd64.inc>` is the Windows SDK's -- the macros for x64
#     prologues, `push_reg`, `alloc_stack`, `end_prologue` -- and llvm-ml
#     expands them once it is told where the SDK keeps them.
#   * `proc public frame` opens no unwind frame in llvm-ml, which wants `frame`
#     straight after `proc`; a `.pushreg` then has nothing to belong to. ml64
#     makes every PROC public by default, so the word says nothing ml64 did not
#     already do, and the build assembles copies with it taken out. Measured:
#     both files assemble, both procedures come out external, and each carries
#     its unwind data.
#
# **And GNU as is kept out of it.** libwavpack chooses MASM by asking `if(MSVC)`,
# which a GNU-driver Clang is not, so it goes on to look for an AT&T assembler
# as well -- and finds Strawberry Perl's `as.exe`, which is on PATH here and on
# CI's runners, and whose language claims `.asm` too. Enabled after MASM, it
# won the extension: the first build here handed it the MASM, which it read as
# a page of syntax errors. An empty compiler is how `check_language` is told
# there is none, and the copies below say their language rather than leaving it
# to the extension.
if(WIN32)
    find_program(MEDIAPERCH_LLVM_ML NAMES llvm-ml llvm-ml.exe
        HINTS "${MEDIAPERCH_LLVM_BIN}" NO_DEFAULT_PATH
        DOC "llvm-ml, which assembles libwavpack's x64 MASM")
    cmake_host_system_information(RESULT mediaperch_kits QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
    file(GLOB mediaperch_ksamd64 "${mediaperch_kits}/Include/*/shared/ksamd64.inc")
    if(MEDIAPERCH_LLVM_ML AND mediaperch_ksamd64)
        # The newest SDK, which is the one Clang takes too.
        list(SORT mediaperch_ksamd64 COMPARE NATURAL ORDER DESCENDING)
        list(GET mediaperch_ksamd64 0 mediaperch_ksamd64)
        get_filename_component(MEDIAPERCH_SDK_SHARED "${mediaperch_ksamd64}" DIRECTORY)
        set(CMAKE_ASM_MASM_COMPILER "${MEDIAPERCH_LLVM_ML}")
        set(CMAKE_ASM_MASM_FLAGS_INIT "-m64")
        enable_language(ASM_MASM)
        set(CMAKE_ASM-ATT_COMPILER "")
        # CMake's MASM defaults ask for `/Zi` in Debug and RelWithDebInfo, and
        # llvm-ml writes no debug information: it says "ignoring unsupported
        # 'Zi' option" once per file. Cleared here, where the files are.
        set(CMAKE_ASM_MASM_FLAGS_DEBUG "")
        set(CMAKE_ASM_MASM_FLAGS_RELWITHDEBINFO "")
        mediaperch_check_llvm_tool("MASM assembler" "${CMAKE_ASM_MASM_COMPILER}"
            "^llvm-ml(\\.exe)?$")
    else()
        message(WARNING
            "llvm-ml or the Windows SDK's ksamd64.inc was not found, so libwavpack "
            "builds without its assembly -- 13% slower, and the same bytes.")
    endif()
endif()
set(WAVPACK_ENABLE_ASM ON CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_LEGACY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/wavpack"
                 "${CMAKE_BINARY_DIR}/external/wavpack" EXCLUDE_FROM_ALL SYSTEM)

mediaperch_add_module(mp_demux_wavpack
    KIND demux
    SOURCES demux_wavpack.cpp
    LINK wavpack)

# Not ours, and not held to our warning set.
if(TARGET wavpack)
    # Guarded by language for the reason cmake/CompilerOptions.cmake gives at
    # length: libwavpack has MASM in it, and a warning level is not a thing an
    # assembler has.
    target_compile_options(wavpack PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-w>")
    set_target_properties(wavpack PROPERTIES FOLDER "external")

    # libwavpack adds -Wall for every language in its directory when the
    # compiler is Clang, and llvm-ml answers it with "ignoring unsupported 'W'
    # option" once per file. The C has -w above, so -Wall reached nothing that
    # listened; it is taken off the target, which copied it from the directory.
    get_target_property(mediaperch_wavpack_options wavpack COMPILE_OPTIONS)
    if(mediaperch_wavpack_options)
        list(REMOVE_ITEM mediaperch_wavpack_options -Wall)
        set_property(TARGET wavpack PROPERTY COMPILE_OPTIONS "${mediaperch_wavpack_options}")
    endif()

    # The copies llvm-ml assembles, in place of the submodule's two x64 files:
    # see the comment above `enable_language`. The sources are listed inside
    # generator expressions upstream, so the path is replaced where it stands.
    if(CMAKE_ASM_MASM_COMPILER AND MEDIAPERCH_SDK_SHARED)
        set(mediaperch_masm_dir "${CMAKE_BINARY_DIR}/external/wavpack/masm")
        get_target_property(mediaperch_wavpack_sources wavpack SOURCES)
        foreach(mediaperch_asm IN ITEMS pack_x64 unpack_x64)
            file(READ "${CMAKE_SOURCE_DIR}/external/wavpack/src/${mediaperch_asm}.asm"
                 mediaperch_text)
            string(REGEX REPLACE "([Pp][Rr][Oo][Cc])[ \t]+[Pp][Uu][Bb][Ll][Ii][Cc][ \t]+([Ff][Rr][Aa][Mm][Ee])"
                   "\\1 \\2" mediaperch_text "${mediaperch_text}")
            # Written only when it differs, so that a configure does not make
            # the assembler run again for nothing. Not file(CONFIGURE): MASM
            # has `@` in it, and that would be read as a variable.
            set(mediaperch_out "${mediaperch_masm_dir}/${mediaperch_asm}.asm")
            set(mediaperch_was "")
            if(EXISTS "${mediaperch_out}")
                file(READ "${mediaperch_out}" mediaperch_was)
            endif()
            if(NOT mediaperch_was STREQUAL mediaperch_text)
                file(WRITE "${mediaperch_out}" "${mediaperch_text}")
            endif()
            list(TRANSFORM mediaperch_wavpack_sources REPLACE
                 "src/${mediaperch_asm}\\.asm" "${mediaperch_masm_dir}/${mediaperch_asm}.asm")
            set_source_files_properties("${mediaperch_masm_dir}/${mediaperch_asm}.asm"
                TARGET_DIRECTORY wavpack
                PROPERTIES LANGUAGE ASM_MASM
                           COMPILE_OPTIONS "-I${MEDIAPERCH_SDK_SHARED}")
        endforeach()
        set_property(TARGET wavpack PROPERTY SOURCES "${mediaperch_wavpack_sources}")
    endif()
endif()

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_wavpack PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_wavpack PRIVATE mediaperch_module_log)

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every compiler and linker flag in the build.
#
# **One toolchain, on every platform: LLVM.** Clang's GNU driver compiles the C
# and the C++, LLD links, llvm-ar archives, and cmake/LLVMToolchain.cmake checks
# at configure time that each of them is LLVM's and of one version. On Windows
# the C++ library is the MSVC STL, linked dynamically -- the one piece of Visual
# Studio a build here still uses, as a library and never as a compiler or a
# linker.
#
# There used to be two blocks in this file, MSVC on Windows and Clang
# everywhere else, sharing no spelling so that each could be read alone. Clang
# was there for the fuzzers and for a Linux head that did not exist yet, and it
# kept finding things MSVC accepted: an enumerator of 0xFFFFFFFF that MSVC
# quietly made -1, a default argument MSVC compiled and the standard does not
# allow, the fields of a settings row that MSVC let a brace list leave out. Two
# compilers meant the modules, most of the code, were only ever read by the one
# that found less. So there is one, and it is the one that found more -- and the
# one that has a C23 compiler behind it rather than `/std:clatest`.
#
# **clang-cl is still refused.** It is Clang wearing MSVC's flag spellings and
# meaning different things by several of them -- /Ob3 maps to a different
# inliner, /Zc:preprocessor is a no-op it warns about, MSVC warning numbers name
# nothing -- so every flag would need a second reading to work out what it
# meant. The GNU driver has one set of words on every platform, and the object
# format is the only thing that differs.
#
# **Scope** is the other decision this file keeps making. Almost everything is
# set with `add_compile_options` at directory scope, which reaches every target
# the build compiles, submodules included -- deliberately: libFLAC, libvorbis,
# libopus and dr_libs are most of the bytes that ship and most of the parsing
# that happens, and hardening or optimising only the tenth of the binary this
# project wrote would be a strange place to stop.
#
# Two kinds of flag cannot be global, and they are the only ones on the
# `mediaperch_flags` interface target:
#
#   * **Warnings.** -Werror on somebody else's source fails a build that has
#     nothing wrong with it, and the failure would be ours rather than theirs.
#   * **Definitions that change what the OS headers mean** -- NOMINMAX, UNICODE.
#     A submodule that calls the ANSI API or uses a `min` macro is not wrong.
#
# Everything here must be set before the first `add_subdirectory`, which is why
# the root list file includes it where it does.

add_library(mediaperch_flags INTERFACE)

# ---------------------------------------------------------------------------
# Which instruction set
# ---------------------------------------------------------------------------
#
# **Two builds, not one binary that decides at run time.**
#
# Everything this tree links already dispatches internally: libFLAC, libmpg123,
# libopus and libwavpack each check the CPU and pick a path. What none of them
# covers is the code *here* -- the resampler, the convolver, the FFT, the
# equaliser, the channel matrix, the dither -- which is Path B's inner loops and
# is compiled to the x86-64 baseline, meaning SSE2 and nothing after 2003.
#
# Raising that baseline is a one-line change and an unshippable one: a binary
# built with AVX2 does not start on a CPU without it. The usual answer is to
# compile the hot loops twice and dispatch, which means a dispatcher, a second
# copy of every stage, and a CPU check on a path that must not branch. The
# answer taken here is to **build the whole tree twice and ship both**, which
# costs a CI job and no code at all. A person picks the one their machine runs;
# `baseline` is the default and the one that runs anywhere.
#
# `avx2` is x86-64-v3: AVX2, FMA, BMI1 and BMI2, LZCNT, MOVBE, F16C -- Haswell
# and Zen and later, which is 2013 onwards. Clang spells the whole set
# `-march=x86-64-v3`, and the baseline `-march=x86-64`, which is given as well
# so that a compiler built with some other default still builds this one.
#
# **This changes floating-point results**, because FMA computes a multiply and
# an add with one rounding where two instructions round twice, and Clang
# contracts `a * b + c` into one wherever a single expression allows it -- which
# C and C++ permit, and which is more accurate rather than less, so nothing here
# turns it off. Measured, it changes bytes in the AVX2 build only: the baseline
# build is MSVC's to the byte over the whole format corpus and 144 Path B runs,
# and the AVX2 build differs in the lossy decoders of three C libraries, one
# noise shaper and a float FFT, while every lossless path stays identical.
# docs/formats.md has the numbers. `-ffp-contract=off` here is the line to
# add if the two builds should hash alike again, as MSVC's did -- unmeasured,
# because nothing here asks for it.
set(MEDIAPERCH_ARCH "baseline" CACHE STRING
    "Instruction-set baseline: baseline (x86-64, SSE2) or avx2 (x86-64-v3)")
set_property(CACHE MEDIAPERCH_ARCH PROPERTY STRINGS baseline avx2)
if(NOT MEDIAPERCH_ARCH MATCHES "^(baseline|avx2)$")
    message(FATAL_ERROR "MEDIAPERCH_ARCH must be baseline or avx2, not ${MEDIAPERCH_ARCH}")
endif()

# ---------------------------------------------------------------------------
# Which toolchain
# ---------------------------------------------------------------------------
#
# Clang's GNU driver and nothing else. Each refusal says what to do instead,
# because the likeliest way to reach one is a configure without a preset.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "MSVC (${CMAKE_CXX_COMPILER}) is not a supported compiler. MediaPerch builds "
        "with LLVM on every platform: Clang's GNU driver and LLD, with the MSVC STL as "
        "the C++ library on Windows. Configure through a preset -- cmake --preset "
        "baseline, or cmake --list-presets -- which names every tool.")
elseif(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "clang-cl is not a supported compiler here; Clang's GNU driver is. It is the "
        "same compiler with MSVC's flag spellings, and every flag in the build would "
        "need a second reading. Configure through a preset, which names clang and "
        "clang++.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "GCC (${CMAKE_CXX_COMPILER}) is not a supported compiler, on any platform. "
        "MediaPerch builds with LLVM's Clang and LLD; configure through a preset.")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "Unrecognised compiler ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). "
        "MediaPerch builds with LLVM's Clang and LLD; configure through a preset.")
endif()

# **LLD links everything.** CMake gives CMAKE_LINKER_TYPE to every target and to
# every try_compile(), which is what makes a configure-time check and the build
# agree about the linker. The presets set it and pass -fuse-ld=lld as well, for
# the builds this one starts by hand (external projects, Meson, cargo).
if(NOT CMAKE_LINKER_TYPE STREQUAL "LLD")
    message(FATAL_ERROR
        "MediaPerch links with LLD, and CMAKE_LINKER_TYPE is '${CMAKE_LINKER_TYPE}'. "
        "Configure through a preset, which sets -D CMAKE_LINKER_TYPE=LLD.")
endif()

include(LLVMToolchain)

# **One driver, two object formats.** The same clang++ produces COFF on Windows
# and ELF on Linux, and the linker options are not the same words: `-z relro` is
# a hardening flag to ld.lld and, to lld-link, an unknown argument followed by a
# file called `relro` -- which is what broke the fuzz job once. The compiler
# options below are one set; the link ones ask this, once.
set(MEDIAPERCH_ELF TRUE)
if(WIN32)
    set(MEDIAPERCH_ELF FALSE)
endif()

# ---------------------------------------------------------------------------
# Warnings, and the definitions that change what a header means
# ---------------------------------------------------------------------------
target_compile_options(mediaperch_flags INTERFACE
    -Wall
    -Wextra
    # **Warnings are errors, everywhere this target reaches** -- which is every
    # target outside external/, by `mediaperch_flags_everywhere` below. A
    # warning that stays a warning is read once and then never, which is how
    # thirteen `strcpy` deprecations and an ignored hash feed sat in the build
    # log for as long as they did.
    -Werror
    -Wconversion
    -Wsign-conversion
    -Wshadow
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
)
if(WIN32)
    target_compile_definitions(mediaperch_flags INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Hardening, global
# ---------------------------------------------------------------------------
#
# **Every global option below is guarded by language, and the reason is an
# assembler.** `add_compile_options` reaches every target in the directory *and
# every language they compile*; libwavpack arrived with hand-written MASM in it,
# and an assembler handed a C compiler's options fails in ways that name nothing
# (`ml64.exe` said `A1004: out of memory`). None of these was ever meant for
# anything but C and C++, and the guard states what was always true.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fstack-protector-strong>")
option(MEDIAPERCH_SANITIZE "Build with the address and undefined-behaviour sanitizers" OFF)
if(MEDIAPERCH_ELF)
    add_link_options(LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
elseif(MEDIAPERCH_SANITIZE OR MEDIAPERCH_BUILD_FUZZERS)
    # **Not under the sanitizers.** AddressSanitizer on Windows reserves its
    # shadow memory and commits it a page at a time from an exception handler,
    # resuming the instruction that touched it -- and with Control Flow Guard,
    # the exception-continuation table and a CET-compatible image around it, a
    # fuzz run reported its own shadow as "access-violation on unknown
    # address", then died printing the report, with no stack and no crash
    # input. Measured on the mp4 fuzzer, one seed, one input: the address was
    # in ASan's HighShadow, and the same run with these flags off finished. A
    # build that exists to observe the program does without them, as it did
    # while MSVC built the rest; the address-space flags stay.
    add_link_options(LINKER:/dynamicbase LINKER:/nxcompat LINKER:/highentropyva)
else()
    # The same protections MSVC's /GS, /guard:cf and /guard:ehcont gave, in
    # Clang's words: the stack cookie above, Control Flow Guard's checks and
    # tables, and the table of valid exception-handling continuations that a
    # CET shadow stack needs before an image can say it is compatible with one.
    # lld-link writes the tables and the image's flags; the last three are its
    # defaults and are stated so that nobody has to know that.
    #
    # **Both are the compiler's own options, through -Xclang.** The GNU driver's
    # `-mguard=cf` is MinGW's -- for `x86_64-pc-windows-msvc` it is "unsupported
    # option", which is what the first build here said, forty times a second --
    # and clang-cl's /guard: spellings are the dialect this file does not use.
    # `-cfguard` and `-ehcontguard` are what both of those turn into.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-Xclang -cfguard>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-Xclang -ehcontguard>")
    add_link_options(LINKER:/guard:cf LINKER:/guard:ehcont LINKER:/cetcompat
                     LINKER:/dynamicbase LINKER:/nxcompat LINKER:/highentropyva)
endif()

# The instruction set, for the whole build including the submodules. A library
# that dispatches internally still works: its baseline path simply becomes
# x86-64-v3 too, and this binary requires it regardless.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    if(MEDIAPERCH_ARCH STREQUAL "avx2")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3>")
    else()
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64>")
    endif()
elseif(NOT MEDIAPERCH_ARCH STREQUAL "baseline")
    message(FATAL_ERROR "MEDIAPERCH_ARCH=${MEDIAPERCH_ARCH} is for x86-64 builds only")
endif()

# ---------------------------------------------------------------------------
# Optimisation, global, Release
# ---------------------------------------------------------------------------
#
# **Every optimisation there is, and no option to take fewer**, except anything
# that trades a result for speed: no -ffast-math and none of its parts that
# change what a floating-point operation computes. What is here:
#
#   * -O3, which is CMake's own Release level for Clang.
#   * One section per function and per datum, so that the linker can see each
#     as a separate thing to discard or to fold (/OPT:REF and /OPT:ICF on COFF,
#     --gc-sections on ELF) -- MSVC's /Gy and /Gw, in Clang's words.
#   * -fno-math-errno: the math functions are not expected to set errno, so the
#     compiler may put an instruction in their place. What they compute stays
#     IEEE's; Clang already assumes it for the Windows C runtime.
#   * -ffinite-loops: a loop whose condition is not a constant is assumed to
#     end, as C11 and C++11 already let it assume -- extended to the C90 of the
#     submodules.
#   * -fomit-frame-pointer: the register is the code's.
add_compile_options(
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-O3>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-ffunction-sections>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fdata-sections>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fno-math-errno>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-ffinite-loops>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fomit-frame-pointer>")
if(MEDIAPERCH_ELF)
    add_link_options("$<$<CONFIG:Release>:LINKER:--gc-sections>")
else()
    add_link_options("$<$<CONFIG:Release>:LINKER:/OPT:REF>"
                     "$<$<CONFIG:Release>:LINKER:/OPT:ICF>")
endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
# (MEDIAPERCH_SANITIZE is declared with the hardening above, which it changes.)

if(MEDIAPERCH_SANITIZE)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=address,undefined>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>")
    add_link_options(-fsanitize=address,undefined)

    # **The release C runtime, in every configuration.** Clang's AddressSanitizer
    # on Windows does not support the debug CRT: with it, a program's first
    # free went to a heap the sanitizer had not allocated from, and every test
    # stopped at start with "attempting free on address which was not
    # malloc()-ed". The Debug configuration keeps its -O0 and its debug
    # information; only the runtime library is the release one.
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()

    # **The runtime is a DLL, and it goes beside the programs.** On Windows
    # AddressSanitizer links its runtime dynamically, and a program that cannot
    # find `clang_rt.asan_dynamic-x86_64.dll` does not start -- Windows says so
    # in a dialog and the test reports 0xC0000135. It used to be found by
    # accident: vcvars64.bat put MSVC's own copy of a DLL by that name on PATH.
    # This is the one of the Clang in use, from its resource directory, as the
    # fuzzers take it (fuzz/CMakeLists.txt), copied into every configuration's
    # output directory now so that a test run finds it with nothing on PATH.
    if(WIN32)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
            OUTPUT_VARIABLE mediaperch_clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(TO_CMAKE_PATH "${mediaperch_clang_resources}" mediaperch_clang_resources)
        set(mediaperch_asan_runtime
            "${mediaperch_clang_resources}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
        if(NOT EXISTS "${mediaperch_asan_runtime}")
            message(FATAL_ERROR
                "MEDIAPERCH_SANITIZE needs ${mediaperch_asan_runtime}, the runtime of "
                "the Clang in use, and it is not there.")
        endif()
        get_property(mediaperch_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(mediaperch_multi_config)
            set(mediaperch_output_dirs "")
            foreach(mediaperch_config IN LISTS CMAKE_CONFIGURATION_TYPES)
                list(APPEND mediaperch_output_dirs
                     "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${mediaperch_config}")
            endforeach()
        else()
            set(mediaperch_output_dirs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        endif()
        foreach(mediaperch_output_dir IN LISTS mediaperch_output_dirs)
            file(COPY "${mediaperch_asan_runtime}" DESTINATION "${mediaperch_output_dir}")
        endforeach()
        message(STATUS "ASan runtime ${mediaperch_asan_runtime}, beside the programs")
    endif()
endif()

# ---------------------------------------------------------------------------
# Link-time optimisation
# ---------------------------------------------------------------------------
#
# Part of what Release *is* rather than a switch: it is what lets the linker see
# that a decoder's inner loop and its caller are one function, and what lets
# /OPT:REF discard the parts of a submodule nothing reaches. With Clang, CMake's
# IPO is ThinLTO (-flto=thin), and LLD runs the optimisation and the code
# generation across modules at link time -- at levels of its own, 2 unless it is
# told otherwise, and Clang passes the compile's -O3 on to it for ELF only. So
# the levels are given to LLD itself.
#
# Two builds do without it, for the same reason: they exist to observe the
# program rather than to be fast, and cross-module inlining moves the frames a
# sanitizer report and a fuzzer crash both point at.
if(NOT MEDIAPERCH_SANITIZE AND NOT MEDIAPERCH_BUILD_FUZZERS)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MEDIAPERCH_IPO_OK OUTPUT MEDIAPERCH_IPO_WHY LANGUAGES C CXX)
    if(NOT MEDIAPERCH_IPO_OK)
        # A hard failure, and deliberately not a fallback. A Release build
        # without link-time optimisation is a different binary from the one this
        # project measures, and the difference would arrive months later looking
        # like an unexplained regression rather than like a build setting.
        message(FATAL_ERROR
            "A Release build here is link-time optimised, and this toolchain "
            "cannot do it:\n${MEDIAPERCH_IPO_WHY}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    if(MEDIAPERCH_ELF)
        add_link_options("$<$<CONFIG:Release>:LINKER:--lto-O3,--lto-CGO3>")
    else()
        add_link_options("$<$<CONFIG:Release>:LINKER:/opt:lldlto=3,/opt:lldltocgo=3>")
    endif()
endif()

# ---------------------------------------------------------------------------
# The two switches that are left
# ---------------------------------------------------------------------------

# The measuring apparatus -- `mediaperch-probe compare`, the loopback verifier
# -- is how every hard bug in this project was found, and none of it is needed
# to play a file. Compiled in for Debug, left out of an optimised build, and put
# back by this. The libraries it lives in are built and unit-tested either way;
# it is only the executable that does without.
option(MEDIAPERCH_DIAGNOSTICS
       "Keep the diagnostic and measuring code in optimised builds too" OFF)

target_compile_definitions(mediaperch_flags INTERFACE
    "$<$<OR:$<CONFIG:Debug>,$<BOOL:${MEDIAPERCH_DIAGNOSTICS}>>:MEDIAPERCH_DIAGNOSTICS=1>")

# "The binary got bigger" is not a finding until it says *what* got bigger. A
# map file attributes every byte of the image to the object it came from, which
# turns a size regression into a name; `tools/mapsize.py` reads one. Off by
# default, because it writes a file beside every binary.
option(MEDIAPERCH_LINK_MAP "Ask the linker for a map file beside every binary" OFF)

if(MEDIAPERCH_LINK_MAP)
    if(MEDIAPERCH_ELF)
        add_link_options(LINKER:-Map=$<TARGET_FILE:$<TARGET_PROPERTY:NAME>>.map)
    else()
        add_link_options(LINKER:/MAP)
    endif()
endif()

# ---------------------------------------------------------------------------
# What an external project is told
# ---------------------------------------------------------------------------
#
# **A build this one starts by hand gets the same toolchain, by name.** aom, avm,
# libde265 and HM are configured by their own CMake as external projects, and an
# external project inherits nothing: left alone it looks for a compiler the way
# any fresh configure does, which on a machine with Visual Studio is cl.exe. So
# every one of them is handed this list -- the compilers, LLD, the archivers and
# the dynamic C runtime -- and cmake/LLVMToolchain.cmake's check has nothing to
# be different from. The instruction set and the optimisation flags are not in
# it: those libraries choose their own, and dispatch on the CPU.
set(MEDIAPERCH_EXTERNAL_CMAKE_ARGS
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_LINKER_TYPE=LLD"
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_AR=${CMAKE_AR}"
    "-DCMAKE_RANLIB=${CMAKE_RANLIB}"
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL")

# ---------------------------------------------------------------------------
# Everything outside external/ gets the flags, without being asked to
# ---------------------------------------------------------------------------
#
# `mediaperch_flags` used to be linked by each target that remembered to. The
# ABI probes had not, and a target that forgets is a target with no warnings at
# all -- which is invisible, because a clean build log and a silent one look
# the same. So the root list file calls this last, and it walks every directory
# configured from inside the source tree, skipping external/, and links the
# flags into every target that compiles something. A target that already had
# them gets them twice, which CMake collapses.
#
# **Somebody else's code compiled inside this tree is marked, not flagged.** A
# few targets exist only to compile somebody else's source -- dr_wav's
# implementation, four files of the VST3 SDK -- and they carry the target
# property MEDIAPERCH_EXTERNAL: the flags are not linked in, and `-w` is added in
# their place, which is what every submodule's own targets are given. MSVC
# compiled those four files quietly; Clang's default warnings found eighteen
# things in `funknown.cpp` -- `%u` for an `unsigned long`, `strcpy` -- that are
# the SDK's to fix and would otherwise be in every build log. The line between
# ours and theirs is a target boundary, so that everything on our side of it is
# held to the full set with no holes.
function(mediaperch_flags_everywhere directory)
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${subdirectory}")
        file(RELATIVE_PATH from_build "${CMAKE_BINARY_DIR}" "${subdirectory}")
        # Outside the tree, under external/, or inside the build directory --
        # which is where FetchContent unpacks Catch2, and which sits *under*
        # the source tree here, so "outside the tree" alone did not exclude
        # it: Catch2 got this tree's flags and its `main` did not survive.
        if(relative MATCHES "^\\.\\." OR relative MATCHES "^external(/|$)"
           OR NOT from_build MATCHES "^\\.\\.")
            continue()
        endif()
        mediaperch_flags_everywhere("${subdirectory}")
    endforeach()
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type "${target}" TYPE)
        get_target_property(theirs "${target}" MEDIAPERCH_EXTERNAL)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        if(theirs)
            target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-w>")
        else()
            target_link_libraries("${target}" PRIVATE mediaperch_flags)
        endif()
    endforeach()
endfunction()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every compiler and linker flag in the build.
#
# **One block per toolchain, and nothing shared between them.** There are two:
# MSVC on Windows, and Clang's GNU driver everywhere else -- which today means
# the fuzzers and tomorrow means the Linux head. Each block says what one
# compiler gets, in full, and can be changed without reading the other. They
# share no spelling, which is the reason there are two and not three: clang-cl
# accepts MSVC's words and means different things by several of them, and every
# flag in the file then needed a second reading to work out which compilers it
# reached.
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
#   * **Warnings.** /W4 on somebody else's source fails a build that has nothing
#     wrong with it, and the failure would be ours rather than theirs.
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
# and Zen and later, which is 2013 onwards. MSVC spells the whole set `/arch:AVX2`
# and Clang spells it `-march=x86-64-v3`.
#
# **This can change floating-point results**, because FMA computes a multiply
# and an add with one rounding where two instructions round twice. Whether it
# does for this tree is a question about bytes rather than about speed, and
# docs/building.md records the answer.
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
# GCC is not one of them. Supporting it would buy a third warning dialect, a
# third set of quirks and a third CI leg for a platform Clang already covers --
# and the one thing it reliably does buy is what happened here: a Ninja
# generator outside a developer prompt finding `cc` on PATH and building the
# whole project with the wrong compiler, cleanly, without saying so.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(MEDIAPERCH_TOOLCHAIN msvc)
elseif(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl is Clang wearing MSVC's flag spellings, and that is exactly the
    # problem: every flag then has to be checked against two compilers that
    # accept the same word and mean different things by it -- /Ob3 maps to a
    # different inliner, /Zc:preprocessor is a no-op it warns about, MSVC
    # warning numbers name nothing. Two toolchains that share no spellings at
    # all are simpler than three that share most of them, and Linux is coming
    # with a real Clang anyway.
    message(FATAL_ERROR
        "clang-cl is not a supported toolchain here. MediaPerch builds with "
        "MSVC on Windows and with Clang's GNU driver elsewhere.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MEDIAPERCH_TOOLCHAIN clang)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "MediaPerch builds with MSVC or Clang. GCC (${CMAKE_CXX_COMPILER}) is "
        "not supported, on any platform.\n"
        "On Windows this almost always means a Ninja preset was configured "
        "outside a developer prompt and picked up MinGW: run "
        "VC/Auxiliary/Build/vcvars64.bat first, or use the `vs` preset.")
else()
    message(FATAL_ERROR
        "Unrecognised compiler ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). "
        "MediaPerch builds with MSVC or Clang.")
endif()

# ---------------------------------------------------------------------------
# MSVC
# ---------------------------------------------------------------------------
if(MEDIAPERCH_TOOLCHAIN STREQUAL msvc)

    # -- warnings, and the definitions that change what a header means -------
    target_compile_options(mediaperch_flags INTERFACE
        /W4
        # **Warnings are errors, everywhere this target reaches** -- which is
        # every target outside external/, by `mediaperch_flags_everywhere`
        # below. A warning that stays a warning is read once and then never,
        # which is how thirteen `strcpy` deprecations and an ignored hash
        # feed sat in the build log for as long as they did.
        /WX
        /permissive-
        # The analyzer's findings are errors too, under the same /WX. Its one
        # false positive in this tree -- a null dereference reported for
        # `new (std::nothrow) T{}` -- was written around, because a waiver is
        # read as advice and the two findings beside it were real.
        /analyze
        # **The analyzer reads external headers, and /external:W0 does not stop
        # it.** A submodule added with `SYSTEM` has its headers passed as
        # `/external:I`, and CMake adds `/external:W0` with them, which is what
        # keeps libebml's sign conversions out of demux_mkv's build -- for the
        # compiler. Measured: with /WX on, the analyzer still reported C6387
        # from inside EbmlBinary.h (a memcpy after an unchecked malloc, real and
        # not this tree's to fix) and failed the build. So the analyzer is told
        # what the compiler already knew. This is the one flag here that exists
        # for somebody else's code, and it says only "not theirs".
        /analyze:external-
        /utf-8
        /Zc:__cplusplus
        /Zc:inline
        /Zc:preprocessor   # the ABI header assumes a conforming preprocessor
        /volatile:iso
        # "structure was padded due to alignment specifier" -- that padding is
        # the point. The ring keeps its two indices on separate cache lines so a
        # thread with a 3 ms deadline does not eat a coherence miss per handover.
        /wd4324
        /w14242            # narrowing conversion
        /w14640            # non-thread-safe static in a member function
        /w14826            # sign-extending conversion
        # C++ only: the ABI header is also compiled as C, and these are noise there.
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        $<$<COMPILE_LANGUAGE:CXX>:/w14263>  # member function does not override anything
        $<$<COMPILE_LANGUAGE:CXX>:/w14265>  # non-virtual destructor on a polymorphic class
    )
    target_compile_definitions(mediaperch_flags INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    # -- hardening, global ---------------------------------------------------
    #
    # /CETCOMPAT is a whole-image property rather than a per-target one: it makes
    # the linker require that every object carrying C++ EH metadata was compiled
    # with /guard:ehcont. As a target-scoped flag it produced LNK2047 on every
    # object of libFLAC, a pure C library, because MSVC emits compound EH
    # metadata for C objects too. "C has no exceptions" was reasoning where a
    # build was available, and it cost two links to find out.
# **Every global option below is guarded by language, and the reason is an
# assembler.**
#
# `add_compile_options` reaches every target in the directory *and every
# language they compile*, which for a tree of C and C++ was a distinction
# without a difference until libwavpack arrived with hand-written MASM in it.
# `ml64.exe` was then handed `/GS /guard:cf /guard:ehcont /Oi /Ot /Gy /Gw /Ob3`,
# understood none of them, and failed with `A1004: out of memory` -- which is
# what that assembler says when it cannot parse its command line, and is as
# unhelpful a message as this build has produced.
#
# So each of these says `$<COMPILE_LANGUAGE:C,CXX>`. None of them was ever meant
# for anything else; the guard states what was always true.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/GS>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/guard:ehcont>")
    add_link_options(/GUARD:CF /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)

    # -- optimisation, global, Release -------------------------------------—-
    add_compile_options(
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Oi>"  # intrinsics rather than calls to memcpy
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Ot>"  # speed where speed and size disagree
        # /Gy and /Gw are what let /OPT:REF and /OPT:ICF do anything at all:
        # without them the linker cannot see a function or a datum as a separate
        # thing to discard or to fold.
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gy>"
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gw>"
    )

    # Inline anything the compiler thinks is worth it, everywhere. Two lines,
    # because there are two ways an /Ob2 gets onto a command line:
    #
    #   * CMake's own Release flags say `/O2 /Ob2 /DNDEBUG`. That /Ob2 is
    #     replaced in the variable, so every file this tree compiles carries
    #     one /Ob3 and nothing for it to override.
    #   * libFLAC's CMakeLists prepends `/O2 /Ob2 /Oi /Ot /Oy` to the Release
    #     flags inside its own directory, where the replacement cannot reach.
    #     The global /Ob3 below lands after it and wins, at the cost of D9025
    #     per libFLAC file -- measured: a command-line warning that /WX does
    #     not promote, and libFLAC has no /WX in any case.
    #
    # The same flag twice is silent; measured as well.
    string(REPLACE "/Ob2" "/Ob3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Ob3>")
    # The instruction set, for the whole build including the submodules. A
    # library that dispatches internally still works: its baseline path simply
    # becomes AVX2 too, and this binary requires AVX2 regardless.
    if(MEDIAPERCH_ARCH STREQUAL "avx2")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>")
    endif()

    add_link_options(
        "$<$<CONFIG:Release>:/OPT:REF>"        # drop what nothing calls
        "$<$<CONFIG:Release>:/OPT:ICF>"        # fold what is identical
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>"
    )

# ---------------------------------------------------------------------------
# Clang, GNU driver
# ---------------------------------------------------------------------------
#
# The Linux head, when there is one, and the fuzzers today.
#
# **One driver, two object formats.** The same clang++ produces ELF on Linux and
# COFF on Windows, and the linker options are not the same words. The compiler
# options below are; only the link ones have to ask. `MEDIAPERCH_ELF` is that
# question asked once, so nothing further down has to remember it.
else()

    set(MEDIAPERCH_ELF TRUE)
    if(WIN32)
        set(MEDIAPERCH_ELF FALSE)
    endif()

    # -- warnings ------------------------------------------------------------
    target_compile_options(mediaperch_flags INTERFACE
        -Wall
        -Wextra
        -Werror
        -Wconversion
        -Wsign-conversion
        -Wshadow
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    )

    # -- hardening, global ---------------------------------------------------
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fstack-protector-strong>")
    if(MEDIAPERCH_ELF)
        add_link_options(LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
    else()
        # lld-link does not know `-z`. It reads `-z relro` as an unknown flag
        # followed by an object file called `relro`, and says exactly that --
        # which is what broke the fuzz job. These are the COFF spellings of the
        # same three ideas, and they are already the defaults for this linker.
        add_link_options(LINKER:/DYNAMICBASE LINKER:/NXCOMPAT LINKER:/HIGHENTROPYVA)
    endif()

    # The instruction set; see MEDIAPERCH_ARCH above. `x86-64-v3` is the same
    # set MSVC calls /arch:AVX2, named rather than enumerated.
    if(MEDIAPERCH_ARCH STREQUAL "avx2")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3>")
    endif()

    # -- optimisation, global, Release ---------------------------------------
    add_compile_options(
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-O3>"
        # One section per function and per datum, so --gc-sections has something
        # to work with: the same trade as /Gy and /Gw on the other two.
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-ffunction-sections>"
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fdata-sections>"
    )
    if(MEDIAPERCH_ELF)
        add_link_options("$<$<CONFIG:Release>:LINKER:--gc-sections>")
    else()
        add_link_options("$<$<CONFIG:Release>:LINKER:/OPT:REF>"
                         "$<$<CONFIG:Release>:LINKER:/OPT:ICF>")
    endif()

endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
option(MEDIAPERCH_SANITIZE "Build with the address and undefined-behaviour sanitizers" OFF)

if(MEDIAPERCH_SANITIZE)
    if(MSVC)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>")
    else()
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=address,undefined>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>")
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Link-time optimisation
# ---------------------------------------------------------------------------
#
# Part of what Release *is* rather than a switch: it is what lets the linker see
# that a decoder's inner loop and its caller are one function, and what lets
# /OPT:REF discard the parts of a submodule nothing reaches.
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
    if(MSVC OR NOT MEDIAPERCH_ELF)
        add_link_options(LINKER:/MAP)
    else()
        add_link_options(LINKER:-Map=$<TARGET_FILE:$<TARGET_PROPERTY:NAME>>.map)
    endif()
endif()

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
# implementation, four files of the VST3 SDK, the registry calls whose SDK
# annotations the analyzer objects to -- and they carry the target property
# MEDIAPERCH_EXTERNAL, which is the whole of their treatment: the flags are not
# linked in, and nothing is added in their place. No warning level of their
# own, no waived analyzer, no per-source options. The line between ours and
# theirs is a target boundary, so that everything on our side of it is held to
# the full set with no holes.
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
        if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$"
           AND NOT theirs)
            target_link_libraries("${target}" PRIVATE mediaperch_flags)
        endif()
    endforeach()
endfunction()

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
        /permissive-
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
    add_compile_options(/GS /guard:cf /guard:ehcont)
    add_link_options(/GUARD:CF /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)

    # -- optimisation, global, Release -------------------------------------—-
    add_compile_options(
        "$<$<CONFIG:Release>:/Oi>"  # intrinsics rather than calls to memcpy and friends
        "$<$<CONFIG:Release>:/Ot>"  # favour speed where speed and size disagree
        # /Gy and /Gw are what let /OPT:REF and /OPT:ICF do anything at all:
        # without them the linker cannot see a function or a datum as a separate
        # thing to discard or to fold.
        "$<$<CONFIG:Release>:/Gy>"
        "$<$<CONFIG:Release>:/Gw>"
        # Inline anything the compiler thinks is worth it. Here rather than
        # rewritten into CMAKE_<LANG>_FLAGS_RELEASE because add_compile_options
        # lands *after* those, so /Ob3 wins wherever it meets an /Ob2 -- and it
        # meets one in every file of libFLAC, whose own CMakeLists prepends
        # `/O2 /Ob2 /Oi /Ot /Oy` to the Release flags.
        #
        # That costs a D9025 per file, "'/Ob3' takes precedence over '/Ob2'",
        # and the warning is correct and expected: /Ob3 is precisely what is
        # wanted, and the only way to silence it is to patch a submodule or to
        # inline less. This project would rather have the noise.
        "$<$<CONFIG:Release>:/Ob3>"
    )
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
        -Wconversion
        -Wsign-conversion
        -Wshadow
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    )

    # -- hardening, global ---------------------------------------------------
    add_compile_options(-fstack-protector-strong)
    if(MEDIAPERCH_ELF)
        add_link_options(LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
    else()
        # lld-link does not know `-z`. It reads `-z relro` as an unknown flag
        # followed by an object file called `relro`, and says exactly that --
        # which is what broke the fuzz job. These are the COFF spellings of the
        # same three ideas, and they are already the defaults for this linker.
        add_link_options(LINKER:/DYNAMICBASE LINKER:/NXCOMPAT LINKER:/HIGHENTROPYVA)
    endif()

    # -- optimisation, global, Release ---------------------------------------
    add_compile_options(
        "$<$<CONFIG:Release>:-O3>"
        # One section per function and per datum, so --gc-sections has something
        # to work with: the same trade as /Gy and /Gw on the other two.
        "$<$<CONFIG:Release>:-ffunction-sections>"
        "$<$<CONFIG:Release>:-fdata-sections>"
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
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
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

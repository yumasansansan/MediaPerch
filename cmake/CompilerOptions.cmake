# SPDX-License-Identifier: GPL-3.0-or-later
#
# One place for every compiler flag, as in DragonPerch, and one place for the
# decision about which compilers exist at all. Targets link `mediaperch_flags`
# rather than setting their own.
#
# Two toolchains, and GCC is not one of them: MSVC and clang-cl on Windows, Clang
# on Linux when there is a Linux head. Supporting GCC as well would buy a third
# warning dialect, a third set of quirks and a third CI leg for a platform Clang
# already covers -- and the one thing it reliably does buy is what happened here:
# a Ninja generator outside a developer prompt finding `cc` on PATH and building
# the whole project with the wrong compiler, cleanly, without saying so.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "MediaPerch builds with MSVC or Clang. GCC (${CMAKE_CXX_COMPILER}) is "
        "not supported, on any platform.\n"
        "On Windows this almost always means a Ninja preset was configured "
        "outside a developer prompt and picked up MinGW: run "
        "VC/Auxiliary/Build/vcvars64.bat first, or use the `vs` preset.")
endif()

add_library(mediaperch_flags INTERFACE)

# An optimised build takes every optimisation there is, and there is no option
# to take fewer. A per-flag switch is a promise to keep every combination of
# them working, and the combination anybody actually ships is the one where they
# are all on -- the rest would be untested configurations wearing the same name.
# So: Release is Release.
#
# **Except anything that trades accuracy for speed.** /fp:fast is the obvious
# one, and it is not here and will not be: it lets the compiler reassociate
# floating point, which is precisely the transformation this whole project
# spends its time proving did not happen. A decoder agrees with FFmpeg to 134 dB
# because nobody reordered its sums. Speed that costs a digit is not speed this
# project wants, at any price.

# ---------------------------------------------------------------------------
# Two scopes, and the difference between them is the whole design of this file.
#
#   * **Policy** -- warning dialect, warnings that must not appear, the C++
#     exception model -- goes on `mediaperch_flags`, which only this project's
#     targets link. Imposing /W4 on libvorbis fails a build that has nothing
#     wrong with it, and the failure is ours rather than theirs.
#
#   * **Optimisation** goes through `add_compile_options` and `add_link_options`
#     at directory scope, so it reaches *everything* the build compiles,
#     submodules included. libFLAC, libvorbis, libopus and dr_libs are most of
#     the bytes that ship; optimising only the tenth of the binary this project
#     wrote would be a strange place to stop.
#
# Both must be set before the first `add_subdirectory`, which is why this file is
# included where it is.
# ---------------------------------------------------------------------------

# The optimised configurations, as a generator expression: this has to be
# decided per configuration rather than at configure time, because the two Ninja
# Multi-Config presets and the Visual Studio one all build several at once.
set(MEDIAPERCH_OPTIMISED "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")
set(MEDIAPERCH_FAST "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>")

# True for cl.exe and for clang-cl, which share the flag spelling.
if(MSVC)
    target_compile_options(mediaperch_flags INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:inline
        # A conforming preprocessor, which the ABI header assumes. clang-cl is
        # always conforming and warns that the flag did nothing, so ask only the
        # compiler that needs asking.
        "$<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>"
        /volatile:iso
        /GS                # stack buffer checks: the parsers read other people's files
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
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    target_link_options(mediaperch_flags INTERFACE
        /GUARD:CF
        /CETCOMPAT
    )

    # Optimisation, at directory scope so the submodules get it too. CMake's own
    # Release flags stop at `/O2 /Ob2 /DNDEBUG`; these come after them and win.
    add_compile_options(
        "$<${MEDIAPERCH_FAST}:/Oi>"   # intrinsics rather than calls to memcpy and friends
        "$<${MEDIAPERCH_FAST}:/Ot>"   # favour speed where speed and size disagree
        # /Gy and /Gw are what make /OPT:REF and /OPT:ICF able to do anything:
        # without them the linker cannot see a function or a datum as a separate
        # thing to fold or discard. They are size flags that read like speed
        # flags, and they are worth more here than either.
        "$<${MEDIAPERCH_OPTIMISED}:/Gy>"
        "$<${MEDIAPERCH_OPTIMISED}:/Gw>"
    )

    # /Ob3 is MSVC's "inline anything you think is worth it", and it replaces the
    # /Ob2 CMake already put in the Release flags rather than joining it: two
    # /Ob settings on one command line is a D9025, which is the warning this
    # project has already been taught once by setting /std by hand. clang-cl
    # accepts the spelling and maps it to a different inliner, so only the
    # compiler it was measured on gets it.
    #
    # It replaces the /Ob2 CMake already put in the optimised flags rather than
    # joining it: two /Ob settings on one command line is a D9025, the same
    # warning this project earned once by setting /std by hand. Not written back
    # to the cache either -- a plain set() shadows it for this directory and
    # everything under it, which is the whole build.
    #
    # clang-cl accepts the spelling and maps it to a different inliner, so only
    # the compiler this was measured on gets it: 4.81 s against 5.38 to decode
    # thirty seconds of AAC, for 6% more binary.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        foreach(config RELEASE RELWITHDEBINFO)
            foreach(lang C CXX)
                string(REPLACE "/Ob2" "/Ob3" _flags "${CMAKE_${lang}_FLAGS_${config}}")
                set(CMAKE_${lang}_FLAGS_${config} "${_flags}")
            endforeach()
        endforeach()
    endif()
    add_link_options(
        "$<${MEDIAPERCH_OPTIMISED}:/OPT:REF>"   # drop what nothing calls
        "$<${MEDIAPERCH_OPTIMISED}:/OPT:ICF>"   # fold what is identical
        "$<${MEDIAPERCH_OPTIMISED}:/INCREMENTAL:NO>"
    )


    # These two are whole-image properties, not per-target ones, so they are the
    # exception to "targets link mediaperch_flags rather than setting their own".
    #
    # /CETCOMPAT makes the linker require that *every* object carrying C++ EH
    # metadata was compiled with /guard:ehcont -- third-party code built in-tree
    # included, and Catch2 never sees an interface library it does not link. As
    # a target-scoped flag it produces LNK2047 on every Catch2 object, which
    # reads like a Catch2 problem and is not one. Directory scope, applied
    # before any subdirectory is added, reaches the whole tree.
    #
    # /guard:ehcont requires /guard:cf, so that one moves too rather than being
    # half-applied.
    add_compile_options(
        /guard:cf  # both cl and clang-cl take this one
        # Not restricted to C++, though it was at first on the reasoning that C
        # has no exceptions. MSVC emits compound EH metadata for C objects too,
        # and /CETCOMPAT then rejects every one of them at link time -- which is
        # how libFLAC, a pure C library, produced four LNK2047s. A whole-image
        # property has to be applied to the whole image, and "has no exceptions"
        # was reasoning where a build was available.
        "$<$<CXX_COMPILER_ID:MSVC>:/guard:ehcont>"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang with the GNU driver: the Linux head, when there is one.
    target_compile_options(mediaperch_flags INTERFACE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    )

    add_compile_options(
        "$<${MEDIAPERCH_FAST}:-O3>"
        # One section per function and per datum, so --gc-sections has something
        # to work with. The same trade as /Gy and /Gw above.
        "$<${MEDIAPERCH_OPTIMISED}:-ffunction-sections>"
        "$<${MEDIAPERCH_OPTIMISED}:-fdata-sections>"
    )
    add_link_options("$<${MEDIAPERCH_OPTIMISED}:LINKER:--gc-sections>")
else()
    message(FATAL_ERROR
        "Unrecognised compiler ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). "
        "MediaPerch builds with MSVC, clang-cl, or Clang.")
endif()

# ------------------------------------------------------------- link-time
option(MEDIAPERCH_LTO "Optimise across translation units in optimised builds" ON)

if(MEDIAPERCH_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MEDIAPERCH_IPO_OK OUTPUT MEDIAPERCH_IPO_WHY_NOT LANGUAGES C CXX)
    if(NOT MEDIAPERCH_IPO_OK)
        # A hard failure, on purpose. Link-time optimisation is most of what
        # separates a release build from a debug build with the assertions off:
        # it is what lets the linker see that a decoder's inner loop and its
        # caller are the same function, and what lets /OPT:REF discard the parts
        # of a submodule nothing reaches. Falling back silently would mean the
        # binary somebody measures is not the binary somebody else measured, and
        # the difference would show up as an unexplained regression months
        # later. Saying so once, here, is cheaper.
        message(FATAL_ERROR
            "Link-time optimisation is not available with this toolchain, and "
            "an optimised build is expected to have it:\n${MEDIAPERCH_IPO_WHY_NOT}\n"
            "Configure with -D MEDIAPERCH_LTO=OFF to build without it, knowing "
            "that the result is not the binary this project measures.")
    endif()
    # Only the optimised configurations. A Debug build with LTO is slower to
    # link and no easier to debug, and RelWithDebInfo keeps its line tables
    # either way.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
endif()

# --------------------------------------------------------------- diagnostics
#
# The measuring apparatus -- `mediaperch-probe compare`, the loopback verifier,
# the bit-accounting a decoder keeps about its own last frame -- is how every
# hard bug in this project was found, and none of it is needed to play a file.
# It is compiled in for Debug, compiled in whenever the tests are built (they
# drive it), and left out of a build that ships.
option(MEDIAPERCH_DIAGNOSTICS
       "Keep the diagnostic and measuring code in optimised builds too" OFF)

target_compile_definitions(mediaperch_flags INTERFACE
    "$<$<OR:$<CONFIG:Debug>,$<BOOL:${MEDIAPERCH_DIAGNOSTICS}>>:MEDIAPERCH_DIAGNOSTICS=1>")

# ----------------------------------------------------------------- link maps
#
# "The binary got bigger" is not a finding until it says *what* got bigger. A
# map file attributes every byte of the image to the object it came from, which
# turns a size regression into a name. `tools/mapsize.py` reads one.
option(MEDIAPERCH_LINK_MAP "Ask the linker for a map file beside every binary" OFF)

if(MEDIAPERCH_LINK_MAP)
    if(MSVC)
        add_link_options(/MAP)
    else()
        add_link_options(LINKER:-Map=$<TARGET_FILE:$<TARGET_PROPERTY:NAME>>.map)
    endif()
endif()

option(MEDIAPERCH_SANITIZE "Build with the address and undefined-behaviour sanitizers" OFF)

if(MEDIAPERCH_SANITIZE)
    if(MSVC)
        target_compile_options(mediaperch_flags INTERFACE /fsanitize=address)
    else()
        target_compile_options(mediaperch_flags INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(mediaperch_flags INTERFACE -fsanitize=address,undefined)
    endif()
endif()

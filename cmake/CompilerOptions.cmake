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
else()
    message(FATAL_ERROR
        "Unrecognised compiler ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). "
        "MediaPerch builds with MSVC, clang-cl, or Clang.")
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

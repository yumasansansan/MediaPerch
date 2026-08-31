# SPDX-License-Identifier: GPL-3.0-or-later
#
# One place for every compiler flag, as in DragonPerch. Targets link
# `mediaperch_flags` rather than setting their own.

add_library(mediaperch_flags INTERFACE)

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
        /guard:cf
        /guard:ehcont
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
else()
    target_compile_options(mediaperch_flags INTERFACE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    )
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

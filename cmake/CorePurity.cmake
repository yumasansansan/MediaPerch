# SPDX-License-Identifier: GPL-3.0-or-later
#
# The portable half is portable: no OS headers, no platform conditionals. That
# rule is only worth anything if something checks it, so this runs as a test
# rather than living in a contributing guide.
#
#   cmake -D MEDIAPERCH_CORE_DIR=<dir>[;<dir>...] -P cmake/CorePurity.cmake
#
# A list, because there are two of them now: src/engine and src/player. The
# *other* rule -- that the engine cannot see the player -- needs no script, and
# that is the point of it being two libraries: src/player is not on the engine's
# include path, so reaching across is a compile error.

if(NOT MEDIAPERCH_CORE_DIR)
    message(FATAL_ERROR "MEDIAPERCH_CORE_DIR is required")
endif()

set(forbidden_includes
    "windows\\.h"
    "winrt/"
    "combaseapi\\.h"
    "objbase\\.h"
    "audioclient\\.h"
    "mmdeviceapi\\.h"
    "audiopolicy\\.h"
    "avrt\\.h"
    "d3d[0-9]"
    "dxgi"
    "d2d1"
    "dcomp\\.h"
    "mfapi\\.h"
    "mfidl\\.h"
    "mfreadwrite\\.h"
    "unistd\\.h"
    "dlfcn\\.h"
    "pthread\\.h"
    "sys/"
    "alsa/"
    "pipewire"
    "wayland-"
)

# A platform conditional in core is the same mistake as a platform header, just
# harder to grep for later.
set(forbidden_macros "_WIN32" "_MSC_VER" "__linux__" "__APPLE__" "__unix__")

set(sources "")
foreach(dir IN LISTS MEDIAPERCH_CORE_DIR)
    # **A directory that is not there is the failure, not an empty answer.**
    # This check passed on nothing for three commits after src/core became
    # src/engine and src/player: the glob found no files, found no violations
    # in them, and reported success. A test that cannot fail is worse than no
    # test, because it is also a claim.
    if(NOT IS_DIRECTORY "${dir}")
        message(FATAL_ERROR
            "core purity: ${dir} is not a directory. Whoever moved it has to "
            "say so here, because this check silently passes on an empty list.")
    endif()
    file(GLOB_RECURSE found
        "${dir}/*.hpp"
        "${dir}/*.cpp"
        "${dir}/*.h"
        "${dir}/*.c")
    if(found STREQUAL "")
        message(FATAL_ERROR "core purity: ${dir} holds no sources")
    endif()
    list(APPEND sources ${found})
endforeach()

set(violations "")

foreach(source IN LISTS sources)
    file(STRINGS "${source}" lines)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")

        foreach(pattern IN LISTS forbidden_includes)
            if(line MATCHES "^[ \t]*#[ \t]*include.*${pattern}")
                list(APPEND violations "${source}:${line_number}: OS header -- ${line}")
            endif()
        endforeach()

        foreach(macro IN LISTS forbidden_macros)
            if(line MATCHES "^[ \t]*#[ \t]*(if|ifdef|ifndef|elif).*${macro}")
                list(APPEND violations "${source}:${line_number}: platform conditional -- ${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH sources source_count)

if(violations)
    message("the portable half is not portable any more:")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message(FATAL_ERROR
        "The engine is built alone in CI precisely so this cannot drift. "
        "Whatever needs the OS belongs in a platform head or a module.")
endif()

message(STATUS "core purity: ${source_count} files, no OS headers, no platform conditionals")

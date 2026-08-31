# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/core is portable: no OS headers, no platform conditionals. That rule is
# only worth anything if something checks it, so this runs as a test rather than
# living in a contributing guide.
#
#   cmake -D MEDIAPERCH_CORE_DIR=<dir> -P cmake/CorePurity.cmake

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

file(GLOB_RECURSE sources
    "${MEDIAPERCH_CORE_DIR}/*.hpp"
    "${MEDIAPERCH_CORE_DIR}/*.cpp"
    "${MEDIAPERCH_CORE_DIR}/*.h"
    "${MEDIAPERCH_CORE_DIR}/*.c")

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
    message("src/core is not portable any more:")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message(FATAL_ERROR
        "The core is built alone in CI precisely so this cannot drift. "
        "Whatever needs the OS belongs in a platform head or a module.")
endif()

message(STATUS "core purity: ${source_count} files, no OS headers, no platform conditionals")

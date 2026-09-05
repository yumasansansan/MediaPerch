# SPDX-License-Identifier: GPL-3.0-or-later
#
# libvpx: VP8 and VP9, by the reference implementation.
#
# **Both decoders, and the reference argument is why.** §7 records it for audio:
# for a codec whose reference implementation *is* what the codec means, the
# reference is worth a dependency -- which is how libFLAC, libvorbis and libopus
# got here, lossy ones included. libvpx is that for VP8 and VP9. It is also the
# decoder this tree can hold another against, the way `av1_cross_test.cpp` holds
# dav1d against libaom, the day a second VP9 decoder exists.
#
# VP9 is not a receding format on the evidence: this machine's GPU carries
# VP9_PROFILE0 and VP9_10BIT_PROFILE2 in its fixed-function block, and Microsoft
# ships a VP9 extension. That makes `codec_mft` a possible second opinion rather
# than a reason to skip the reference.
#
# **Three tools in sequence, and none of them new.** libvpx has neither CMake
# nor Meson: `configure` is a POSIX shell script, `make` generates Visual Studio
# project files from what it produced, and MSBuild builds them. The shell is Git
# for Windows' -- git is already a hard requirement here, so MSYS2 is not
# needed and was never needed. NASM is the same one external/dav1d and
# external/aom use. GNU make is the one addition, and nmake cannot stand in:
# libvpx's makefiles use `ifeq`, `$(foreach)` and pattern rules, and its README
# says GNU make in as many words.
#
# The build itself is vpx_build.cmake, which is where the mechanics are.

# **Off by default, because it does not link yet.** Everything below runs:
# configure produces makefiles, make generates a Visual Studio solution, MSBuild
# builds it, and `vpxmd.lib` appears. What comes out is **incomplete** -- 73
# objects holding only `vp8_*` and `vp9_*`, with `vpx_dsp`, `vpx_mem`,
# `vpx_ports` and `vpx_scale` missing, so the archive cannot even resolve its
# own `vpx_calloc`. The same libvpx configured by hand in a directory beside its
# source produces 109 objects and links.
#
# So the difference is either the options added since that first attempt
# (`--disable-unit-tests --disable-webm-io --disable-libyuv`) or the deep
# relative path from this build directory back to external/libvpx, which
# libvpx's project generator prints an "Ignored files list" about. **Which of
# the two is not yet established**, and guessing at a build system is how the
# libaom nesting cost three attempts.
#
# Gated rather than deleted: the module, the ABI values and the Matroska codec
# ids are all written and are not the part that is unfinished.
option(MEDIAPERCH_BUILD_VPX
    "Build codec_vpx. Off: libvpx's generated solution produces an incomplete library and the cause is not yet established -- see modules/codec/vpx." OFF)

if(NOT MEDIAPERCH_BUILD_VPX)
    return()
endif()

set(vpx_src "${CMAKE_SOURCE_DIR}/external/libvpx")

if(NOT EXISTS "${vpx_src}/configure")
    message(WARNING
        "external/libvpx is missing -- run `git submodule update --init`. "
        "Skipping codec_vpx; the tree still builds and simply has no VP8 or VP9 "
        "decoder.")
    return()
endif()

if(NOT MSVC)
    # The `-vs` targets and MSBuild are the whole of this file's build. A Linux
    # head would use libvpx's own makefiles, which is a different twenty lines
    # and belongs with the head that needs them.
    return()
endif()

find_program(MEDIAPERCH_BASH NAMES bash bash.exe
    HINTS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramW6432}/Git/bin"
    DOC "A POSIX shell for libvpx's configure. Git for Windows ships one.")
find_program(MEDIAPERCH_MAKE NAMES make make.exe mingw32-make mingw32-make.exe
    DOC "GNU make, which turns libvpx's configure output into a solution.")
find_program(MEDIAPERCH_NASM NAMES nasm nasm.exe
    DOC "NASM, which assembles libvpx. external/dav1d and external/aom need it too.")

if(NOT MEDIAPERCH_BASH OR NOT MEDIAPERCH_MAKE OR NOT MEDIAPERCH_NASM)
    message(WARNING
        "libvpx needs a POSIX shell, GNU make and nasm, and at least one is "
        "missing. Skipping codec_vpx -- the tree still builds and simply has no "
        "VP8 or VP9 decoder. Git for Windows provides the shell; see "
        "modules/codec/dav1d/CMakeLists.txt for the other two.")
    return()
endif()

# **The Visual Studio generation libvpx targets, from the compiler in hand.**
# Named rather than guessed, because a `-vs17` project opened by a v18 toolchain
# is a retarget prompt rather than a build. MSVC 19.5x is Visual Studio 2026,
# which libvpx calls vs18.
if(MSVC_VERSION GREATER_EQUAL 1950)
    set(vpx_target "x86_64-win64-vs18")
elseif(MSVC_VERSION GREATER_EQUAL 1930)
    set(vpx_target "x86_64-win64-vs17")
else()
    set(vpx_target "x86_64-win64-vs16")
endif()

include(ExternalProject)

set(vpx_prefix "${CMAKE_BINARY_DIR}/external/libvpx")
set(vpx_library "${vpx_prefix}/build/x64/Release/vpxmd.lib")

ExternalProject_Add(vpx_external
    SOURCE_DIR "${vpx_src}"
    BINARY_DIR "${vpx_prefix}/build"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E true
    BUILD_COMMAND "${CMAKE_COMMAND}"
        "-Dbash=${MEDIAPERCH_BASH}"
        "-Dmake=${MEDIAPERCH_MAKE}"
        "-Dsrc=${vpx_src}"
        "-Dbuild=${vpx_prefix}/build"
        "-Dtarget=${vpx_target}"
        "-Dchecks=$<BOOL:${MEDIAPERCH_DECODER_CHECKS}>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/vpx_build.cmake"
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E true
    BUILD_BYPRODUCTS "${vpx_library}"
    # Everything is in the build step, so it is the one that has to run: see
    # vpx_build.cmake, and modules/codec/dav1d for what not re-running cost.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)

add_library(vpx::vpx STATIC IMPORTED GLOBAL)
set_target_properties(vpx::vpx PROPERTIES
    IMPORTED_LOCATION "${vpx_library}"
    # libvpx installs nothing here; its public headers are `vpx/vpx_decoder.h`
    # and friends, at the root of its own tree.
    INTERFACE_INCLUDE_DIRECTORIES "${vpx_src}")
add_dependencies(vpx::vpx vpx_external)

mediaperch_add_module(mp_codec_vpx
    KIND codec
    SOURCES codec_vpx.cpp
    LINK vpx::vpx)

# `vpxmd.lib` is the dynamic release CRT by libvpx's own naming, and the module
# is pinned to match -- §4's rule that nothing is allocated across the module
# boundary is what makes one release library safe for every configuration. The
# same argument external/dav1d's build records at more length.
set_target_properties(mp_codec_vpx PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

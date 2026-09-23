# SPDX-License-Identifier: GPL-3.0-or-later
#
# libvpx: VP8 and VP9, by the reference implementation.
#
# **Both decoders, and the reference argument is why.** §7 records it for audio:
# where a reference implementation *is* what the codec means, it is worth a
# dependency -- which is how libFLAC, libvorbis and libopus got here, the lossy
# ones included. libvpx is that for VP8 and VP9. VP8 is not an afterthought:
# reading only the newer of the two would leave half the reason it is here.
#
# VP9 is not a receding format on the evidence either. This machine's GPU
# carries VP9_PROFILE0 and VP9_10BIT_PROFILE2 in its fixed-function block and
# Microsoft ships a VP9 extension, which makes `codec_mft` a possible second
# opinion the day someone wants one -- the way libaom is dav1d's.
#
# **MSYS2, and it is a real dependency.** libvpx has neither CMake nor Meson:
# `configure` is a POSIX shell script and the build is its own makefiles. For
# 64-bit Windows it knows two ways -- `-vs` targets, which generate Visual
# Studio projects for MSBuild and MSVC, and `x86_64-win64-gcc`, which is its
# makefiles driving whatever CC names -- and the second takes Clang as it takes
# GCC. Clang's GNU driver targets the MSVC ABI on Windows, so what comes out is
# COFF for the same C runtime as everything else here; vpx_build.sh has the
# rest. The part that is not optional is *which* Unix: libvpx's scripts are
# written against MSYS2's sed, cut and make, and a native Windows make runs
# commands through cmd.exe, whose 8191-character limit once truncated this
# build's source list into an archive that could not resolve its own
# `vpx_calloc`. MSYS2's make runs them through its own bash and they fit.
#
# So this is the one submodule whose build wants a second Unix userland rather
# than one more tool. It is skipped, loudly, when that is not there, and the
# tree still builds -- with no VP8 or VP9 decoder, which is what that costs.

set(vpx_src "${CMAKE_SOURCE_DIR}/external/libvpx")

if(NOT EXISTS "${vpx_src}/configure")
    message(WARNING
        "external/libvpx is missing -- run `git submodule update --init`. "
        "Skipping codec_vpx; the tree still builds and simply has no VP8 or VP9 "
        "decoder.")
    return()
endif()

if(NOT WIN32)
    # The Windows build of libvpx's own makefiles. A Linux head would run the
    # same configure with its own target and belongs with the head that needs it.
    return()
endif()

# **MSYS2's bash by name.** Git for Windows ships a bash and it is not the same
# thing: it has no make at all, and its sed and cut are not the ones libvpx's
# scripts were written against. NO_DEFAULT_PATH so that finding Git's instead is
# impossible rather than unlikely.
find_program(MEDIAPERCH_MSYS_BASH
    NAMES bash.exe
    HINTS "C:/msys64/usr/bin" "C:/msys32/usr/bin" "$ENV{MSYS2_ROOT}/usr/bin"
    NO_DEFAULT_PATH
    DOC "MSYS2's bash, which is the only environment libvpx's Windows build works in.")
find_program(MEDIAPERCH_MSYS_MAKE
    NAMES make.exe
    HINTS "C:/msys64/usr/bin" "C:/msys32/usr/bin" "$ENV{MSYS2_ROOT}/usr/bin"
    NO_DEFAULT_PATH
    DOC "MSYS2's make. A native Windows make runs libvpx's long command lines through cmd.exe, which cuts them short.")
find_program(MEDIAPERCH_NASM NAMES nasm nasm.exe
    DOC "NASM, which assembles libvpx. external/dav1d and external/aom need it too.")

if(NOT MEDIAPERCH_MSYS_BASH OR NOT MEDIAPERCH_MSYS_MAKE OR NOT MEDIAPERCH_NASM)
    message(WARNING
        "libvpx needs MSYS2 -- its bash and its make, not Git for Windows' -- and "
        "nasm. At least one is missing, so codec_vpx is skipped. The tree still "
        "builds and simply has no VP8 or VP9 decoder. `pacman -S make` inside "
        "MSYS2 is the usual gap; see modules/codec/vpx/vpx_build.sh for why a "
        "native make will not do.")
    return()
endif()

include(ExternalProject)

set(vpx_prefix "${CMAKE_BINARY_DIR}/external/libvpx")
set(vpx_library "${vpx_prefix}/libvpx.a")

# The tools by path, from the toolchain this build was checked against: the
# makefiles run under MSYS2 and would otherwise find whatever its PATH offers.
# vpx_build.sh puts their directory first on PATH and names them bare.
get_filename_component(mediaperch_llvm_strip "${CMAKE_AR}" DIRECTORY)
set(mediaperch_llvm_strip "${mediaperch_llvm_strip}/llvm-strip${CMAKE_EXECUTABLE_SUFFIX}")

ExternalProject_Add(vpx_external
    SOURCE_DIR "${vpx_src}"
    BINARY_DIR "${vpx_prefix}"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E true
    BUILD_COMMAND "${MEDIAPERCH_MSYS_BASH}"
        "${CMAKE_CURRENT_SOURCE_DIR}/vpx_build.sh"
        "${vpx_src}" "${vpx_prefix}"
        "$<IF:$<BOOL:${MEDIAPERCH_DECODER_CHECKS}>,1,0>"
        "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}" "${CMAKE_AR}"
        "${mediaperch_llvm_strip}" "${MEDIAPERCH_NASM}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E true
    BUILD_BYPRODUCTS "${vpx_library}"
    # Everything is in the build step, so it is the one that has to run -- and
    # the submodule can move under an external project without ExternalProject
    # noticing, which cost a set of measurements against the wrong commit in
    # modules/codec/dav1d.
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

# The archive is compiled for the dynamic release CRT (vpx_build.sh says so to
# Clang), and the module is pinned to match -- §4's rule that nothing is
# allocated across the module boundary is what makes one release library safe
# for every configuration. The same argument external/dav1d's build records at
# more length.
set_target_properties(mp_codec_vpx PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# `threads = 0` is one thread in libvpx, not "the library picks" -- see
# modules/shared/decoder_threads.
target_link_libraries(mp_codec_vpx PRIVATE mediaperch_decoder_threads)

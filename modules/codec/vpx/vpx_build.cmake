# SPDX-License-Identifier: GPL-3.0-or-later
#
# libvpx's build, which is a shell script, a makefile and MSBuild in sequence.
#
# Driven from modules/codec/vpx/CMakeLists.txt; the argument for the whole
# arrangement is there. This file is the mechanics:
#
#   1. `configure` is `#!/bin/sh` and needs a POSIX shell. Git for Windows ships
#      one -- MSYS2-derived -- and git is already a hard requirement of this
#      tree, so no new tool is involved and MSYS2 is not needed.
#   2. `make` does not build anything here. On a `-vs` target it *generates*
#      `vpx.vcxproj` and `vpx.sln` and then calls MSBuild on them, so GNU make
#      is a build-time dependency even though nothing it produces is a makefile
#      build. nmake cannot stand in: the makefiles use `ifeq`, `$(foreach)` and
#      pattern rules, and libvpx's own README says GNU make.
#   3. MSBuild comes with Visual Studio and is on PATH inside a developer
#      environment, which every build of this tree already requires.
#
# **Configured once, built every time.** `configure` is slow and its answer only
# changes when the submodule moves or the options here do, so it is skipped when
# its output is already present; `make` is cheap when nothing changed and is the
# only thing that notices a moved submodule -- see the same argument in
# modules/codec/dav1d/CMakeLists.txt, where not noticing cost a set of
# measurements taken against the wrong commit.

foreach(required bash make src build target checks)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "vpx_build.cmake needs -D${required}=")
    endif()
endforeach()

file(MAKE_DIRECTORY "${build}")

if(NOT EXISTS "${build}/config.mk")
    # **VP8 and VP9 decoders, both**, because libvpx is the reference for both
    # and reading only the newer one would leave half the reason it is here.
    # The encoders go: nothing in this tree encodes.
    #
    # `--enable-vp9-highbitdepth` is off by default and is what makes VP9
    # profiles 2 and 3 -- ten and twelve bits, 4:2:2 and 4:4:4 -- readable at
    # all. Leaving it off would mean ABI v4 could describe depths this decoder
    # then refused to produce.
    #
    # `webm-io` and `libyuv` are on by default and are both jobs this tree does
    # itself: containers are demuxers here, and colour conversion is a shader.
    set(options
        "--target=${target}"
        --as=nasm
        --enable-vp9-highbitdepth
        --disable-vp8-encoder
        --disable-vp9-encoder
        --disable-examples
        --disable-tools
        --disable-docs
        --disable-install-docs
        --disable-unit-tests
        --disable-webm-io
        --disable-libyuv)

    if(checks)
        # **The decoder checks its own intermediate results.** Slower, and what
        # a build being fuzzed or audited wants rather than one being shipped --
        # which is why it is a switch and not the default. See
        # MEDIAPERCH_DECODER_CHECKS.
        list(APPEND options --enable-coefficient-range-checking)
    endif()

    # A relative path to configure, because it is run from the build directory
    # and a Windows drive letter is not a path a POSIX shell reads the same way.
    file(RELATIVE_PATH from_build "${build}" "${src}/configure")

    execute_process(
        COMMAND "${bash}" "${from_build}" ${options}
        WORKING_DIRECTORY "${build}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "libvpx's configure failed (${result})")
    endif()
endif()

# **`make` needs a POSIX shell on PATH, and not just to run one.** GNU make on
# Windows uses `sh.exe` as its shell when it can find one and cmd.exe when it
# cannot -- and libvpx's rules are shell scripts, so with cmd.exe they fail as
#
#     t was unexpected at this time.
#     make: *** [Makefile:17: .DEFAULT] Error 255
#
# which says nothing about shells at all. The directory holding the bash that
# ran `configure` holds `sh.exe` beside it, so putting it in front of PATH is
# the whole fix.
get_filename_component(shell_dir "${bash}" DIRECTORY)
set(ENV{PATH} "${shell_dir};$ENV{PATH}")

# Generates the project files and calls MSBuild on them. Both configurations get
# built, because that is what libvpx's generated rules do and there is no switch
# for it; only the Release one is linked against.
execute_process(
    COMMAND "${make}"
    WORKING_DIRECTORY "${build}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "libvpx's make failed (${result})")
endif()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where HM left the decoder, copied to where this tree can name it.
#
# **HM's build writes under `bin/<generator>/<compiler>-<version>/<arch>/<config>/`**,
# which is BBuildEnv's convention -- in HM's source tree, until
# external/patches/hm-outputs-where-the-build-is-told.patch let the build say
# where instead, so that two builds of one source tree, two instruction sets or
# Windows and Linux from one checkout, stop writing one file. The path carries
# the compiler's version number in it, which changes when the toolchain does, so
# it cannot be written down here; it can be looked for, and only for this
# platform's name of the program.
#
# One file, found and copied, so everything downstream has a fixed path.
#
# Arguments: -Dout=<BBuildEnv_OUTPUT_ROOT> -Ddest=<the copy> -Dsuffix=<.exe or nothing>

file(GLOB_RECURSE found "${out}/bin/*/TAppDecoder${suffix}")
if(NOT found)
    message(FATAL_ERROR
        "HM built without error and left no TAppDecoder${suffix} under ${out}/bin. "
        "Its output convention is BBuildEnv's and may have changed.")
endif()
list(GET found 0 first)
# Newest wins when a toolchain change has left more than one behind.
foreach(one IN LISTS found)
    if("${one}" IS_NEWER_THAN "${first}")
        set(first "${one}")
    endif()
endforeach()
file(COPY_FILE "${first}" "${dest}" ONLY_IF_DIFFERENT)
message(STATUS "HM decoder: ${first}")

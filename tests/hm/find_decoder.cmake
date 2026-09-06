# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where HM left the decoder, copied to where this tree can name it.
#
# **HM's build writes into its own source tree**, under
# `bin/<generator>/<compiler>-<version>/<arch>/<config>/`, which is BBuildEnv's
# convention and is in HM's own `.gitignore` -- so the submodule stays clean and
# the path carries the compiler's version number in it. That number changes when
# the toolchain does, so it cannot be written down here; it can be looked for.
#
# One file, found and copied, so everything downstream has a fixed path.

file(GLOB_RECURSE found "${src}/bin/*/TAppDecoder.exe" "${src}/bin/*/TAppDecoder")
if(NOT found)
    message(FATAL_ERROR
        "HM built without error and left no TAppDecoder under ${src}/bin. Its "
        "output convention is BBuildEnv's and may have changed.")
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

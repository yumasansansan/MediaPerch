# SPDX-License-Identifier: GPL-3.0-or-later
#
# The 4K HEVC fixture, generated rather than committed.
#
# **Six megabytes, against fourteen kilobytes for every other fixture here.**
# The small ones are in `tests/data` because a test that needs a file needs the
# file; this one is three hundred and fifty times larger than the next biggest
# and would be in every clone forever to be used by one measurement. So it is
# generated, the way `format_matrix` generates one file per format, and this
# script is what makes the measurement repeatable rather than an anecdote.
#
#   cmake -D out=<path> -P tests/data/make_4k_hevc.cmake
#
# **It has an audio track on purpose.** M6's acceptance is "4K HEVC plays with
# frames dropped against audio, never the reverse", which is a statement about
# two clocks; a file with no audio has nothing to be dropped *against* and the
# wall clock stands in, which measures the wrong thing.
#
# The numbers are chosen to make the decode hard rather than long: 3840x2160 at
# 23.976, three seconds, a keyframe every second, and `veryfast` so the encode
# is minutes rather than an afternoon. `testsrc2` moves in every frame, which is
# what stops an encoder producing a stream that decodes trivially.

if(NOT DEFINED out)
    message(FATAL_ERROR "usage: cmake -D out=<path.mp4> -P make_4k_hevc.cmake")
endif()

find_program(ffmpeg NAMES ffmpeg ffmpeg.exe)
if(NOT ffmpeg)
    message(FATAL_ERROR
        "ffmpeg is not on PATH. It is not shipped with this tree and is not a "
        "build dependency -- it is how the fixtures are made, the same way "
        "format_matrix uses it.")
endif()

execute_process(
    COMMAND "${ffmpeg}" -v error -y
            -f lavfi -i "testsrc2=size=3840x2160:rate=24000/1001:duration=3"
            -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3"
            -c:v libx265 -preset veryfast -crf 28
            -x265-params "keyint=24:min-keyint=24:log-level=none"
            -pix_fmt yuv420p -tag:v hvc1
            -c:a aac -b:a 96k -shortest
            "${out}"
    RESULT_VARIABLE made)
if(NOT made EQUAL 0)
    message(FATAL_ERROR "ffmpeg would not build the fixture: ${made}")
endif()
message(STATUS "4K HEVC fixture: ${out}")

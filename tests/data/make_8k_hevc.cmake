# SPDX-License-Identifier: GPL-3.0-or-later
#
# The 8K HEVC fixture, generated rather than committed.
#
# **Eleven megabytes, and four times the pixels of the 4K one.** Generated for
# the same reason and more so: a fixture nobody would want in a clone, used by
# one measurement, kept repeatable by this script rather than by an anecdote.
#
# `ultrafast` and crf 30 where the 4K script uses `veryfast` and 28, because 33
# megapixels a frame is where an encode stops being minutes. The point is a
# stream that is expensive to *decode*, and 8K is that whatever the preset.
#
#   cmake -D out=<path> -P tests/data/make_8k_hevc.cmake
#
# **It has an audio track on purpose.** M6's acceptance is "4K HEVC plays with
# frames dropped against audio, never the reverse", which is a statement about
# two clocks; a file with no audio has nothing to be dropped *against* and the
# wall clock stands in, which measures the wrong thing.
#
# The numbers are chosen to make the decode hard rather than long: 7680x4320 at
# 23.976, three seconds, a keyframe every second. `testsrc2` moves in every
# frame, which is what stops an encoder producing a stream that decodes
# trivially.

if(NOT DEFINED out)
    message(FATAL_ERROR "usage: cmake -D out=<path.mp4> -P make_8k_hevc.cmake")
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
            -f lavfi -i "testsrc2=size=7680x4320:rate=24000/1001:duration=3"
            -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3"
            -c:v libx265 -preset ultrafast -crf 30
            -x265-params "keyint=24:min-keyint=24:log-level=none"
            -pix_fmt yuv420p -tag:v hvc1
            -c:a aac -b:a 96k -shortest
            "${out}"
    RESULT_VARIABLE made)
if(NOT made EQUAL 0)
    message(FATAL_ERROR "ffmpeg would not build the fixture: ${made}")
endif()
message(STATUS "4K HEVC fixture: ${out}")

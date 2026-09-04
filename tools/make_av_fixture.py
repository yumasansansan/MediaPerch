# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fixtures with two streams in them, and why they are committed rather than
# built at test time.
#
# `av.mp4` is what FFmpeg writes: one H.264 track and one AAC track, 128x96 and
# one second, small enough to live in the repository. It is committed because
# the property it tests -- that one demuxer serves two streams out of one file
# -- is a correctness property of the ABI rather than a quality measurement, so
# it belongs in the default test run and must not depend on FFmpeg being on the
# machine.
#
# `av_bt2020.mp4` is that file with **four bytes changed**: the `colr` box's
# primaries, transfer and matrix, and the full-range flag. FFmpeg would not
# write them -- `-color_primaries bt2020 -color_trc smpte2084` and the matching
# x265 params both came back as 2/2/2 from this build's muxer, twice -- and the
# thing under test is whether `demux_mp4` reports what the container says. So
# the container is made to say it, by hand, which is also the only way the two
# files differ in exactly one thing.
#
#   python tools/make_av_fixture.py
#
# Run from the repository root. Needs `ffmpeg` on PATH for the first file; the
# second is made from the first and needs nothing.

import io
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP4 = os.path.join(HERE, "tests", "data", "mp4")
MKV = os.path.join(HERE, "tests", "data", "mkv")

# BT.2020 primaries, the PQ transfer, non-constant-luminance BT.2020 matrix, and
# full range -- the ISO/IEC 23091-2 code points, which is what MpVideoInfo
# carries and what §9.1 turns on.
BT2020 = (9, 16, 9, True)


def build_av(path):
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1",
         "-c:v", "libx264", "-preset", "veryfast", "-crf", "40",
         "-pix_fmt", "yuv420p", "-g", "6",
         "-c:a", "aac", "-b:a", "32k",
         # +write_colr so there is a colr box to retag. Its contents are 2/2/1
         # whatever is asked for -- see the header comment.
         "-movflags", "+faststart+write_colr", path],
        check=True)


def retag(src, dst, primaries, transfer, matrix, full_range):
    data = bytearray(io.open(src, "rb").read())
    at = data.find(b"colr")
    if at < 0:
        raise SystemExit("%s has no colr box; the fixture needs one" % src)
    if bytes(data[at + 4:at + 8]) != b"nclx":
        raise SystemExit("colr is not nclx, so there are no code points in it")
    struct.pack_into(">HHH", data, at + 8, primaries, transfer, matrix)
    data[at + 14] = 0x80 if full_range else 0x00
    io.open(dst, "wb").write(bytes(data))


def build_mkv(path):
    # Opus rather than AAC, so the Matroska fixture also exercises the
    # BlockGroup path: the last block of an Opus track carries DiscardPadding,
    # which is the one thing this demuxer reads out of a group rather than a
    # SimpleBlock.
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1",
         "-c:v", "libx264", "-preset", "veryfast", "-crf", "40",
         "-pix_fmt", "yuv420p", "-g", "6",
         "-c:a", "libopus", "-b:a", "32k", path],
        check=True)


def main():
    os.makedirs(MP4, exist_ok=True)
    os.makedirs(MKV, exist_ok=True)
    av = os.path.join(MP4, "av.mp4")
    build_av(av)
    retag(av, os.path.join(MP4, "av_bt2020.mp4"), *BT2020)
    build_mkv(os.path.join(MKV, "av.mkv"))
    for directory, name in ((MP4, "av.mp4"), (MP4, "av_bt2020.mp4"), (MKV, "av.mkv")):
        p = os.path.join(directory, name)
        print("%-16s %d bytes" % (name, os.path.getsize(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
# `av1.mp4` is the same picture and the same audio in AV1 rather than H.264 --
# an `av01` sample entry with an `av1C` record in it, which is the shape
# `demux_mp4` has to recognise before anything can decode AV1 at all. It is the
# same 128x96 and the same twenty-four frames, so a test can compare the two
# containers rather than two different pictures. SVT-AV1 encodes it: libaom is
# the reference and is minutes rather than seconds at these settings, and what
# is under test is the container, not the encoder.
#
# `av1_grain.mp4` is the same again with **film grain**, and it exists for the
# one place two conformant decoders may legitimately disagree. Grain synthesis
# is part of AV1's decoding process and is also the one stage every decoder can
# be told to skip, so a byte-for-byte comparison of two decoders means something
# different on a grainy stream than on a clean one -- and `av1.mp4` has none, so
# without this the cross-check never touches the case. libaom encodes it,
# because `-denoise-noise-level` is how grain parameters get into a stream at
# all; dav1d confirms the result carries them.
#
#   python tools/make_av_fixture.py
#
# Run from the repository root. Needs `ffmpeg` on PATH for the encoded files;
# `av_bt2020.mp4` is made from `av.mp4` and needs nothing.

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


def build_av1(path):
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1",
         # preset 10 and a high CRF because the file is committed and the test
         # is about the container. -g 6 matches av.mp4 so both have the same
         # keyframe spacing.
         "-c:v", "libsvtav1", "-preset", "10", "-crf", "55",
         "-pix_fmt", "yuv420p", "-g", "6",
         "-c:a", "aac", "-b:a", "32k",
         "-movflags", "+faststart+write_colr", path],
        check=True)


def build_av1_grain(path):
    # -denoise-noise-level removes noise from the source and puts the
    # parameters to synthesise it back into the stream, which is exactly the
    # film grain path. cpu-used 8 because libaom is the reference encoder and
    # slow, and what is under test is the grain flag rather than the quality.
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1",
         "-c:v", "libaom-av1", "-cpu-used", "8", "-crf", "45",
         "-denoise-noise-level", "25",
         "-pix_fmt", "yuv420p", "-g", "6",
         "-c:a", "aac", "-b:a", "32k",
         "-movflags", "+faststart+write_colr", path],
        check=True)


def build_pcm_float_mkv(path):
    # **Float PCM in Matroska**, which this tree refused to name until the
    # reasoning was looked at: `codec_pcm` is a memcpy and a memcpy is correct
    # for float, so the only thing that had ever stopped it was a comment.
    # `demux_wav` had been reporting float WAV as MP_CODEC_PCM all along.
    #
    # Deliberately tiny -- a quarter second of mono at 8 kHz -- because
    # uncompressed audio is committed at its full size and what is under test is
    # one CodecID and one sample type.
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=0.25",
         "-c:a", "pcm_f32le", path],
        check=True)


def build_webm(path, encoder):
    # **VP8 and VP9 in WebM**, which is the container both were designed for and
    # the one `demux_mkv` names them in. Small on purpose: what is under test is
    # a CodecID, a decoder and a planar frame, not a picture.
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
         "-c:v", encoder, "-b:v", "60k", "-deadline", "realtime", "-cpu-used", "8",
         "-pix_fmt", "yuv420p", "-g", "6",
         "-c:a", "libopus", "-b:a", "32k", path],
        check=True)


def build_av2(path):
    # **AV2, which ffmpeg cannot encode**: there is one AV2 encoder in the world
    # and it is avm's own, so this one fixture is made by a tool the other
    # builders here do not need. MEDIAPERCH_AVMENC names it; without it the AV2
    # fixture is left alone, since it is already in the tree and a person
    # regenerating the MP4s has no reason to have built avm.
    #
    # WebM, with the CodecID avm itself writes -- see modules/demux/mkv, where
    # V_AV2 is read for exactly that reason. Sixteen frames rather than
    # twenty-four: the reference encoder is minutes where libvpx is seconds, and
    # what is under test is a CodecID, a decoder and a planar frame.
    avmenc = os.environ.get("MEDIAPERCH_AVMENC")
    if not avmenc or not os.path.exists(avmenc):
        print("%-16s skipped: set MEDIAPERCH_AVMENC to avm's encoder"
              % os.path.basename(path))
        return
    y4m = path + ".y4m"
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=24000/1001:duration=1",
         "-frames:v", "16", "-pix_fmt", "yuv420p", y4m],
        check=True)
    try:
        subprocess.run(
            [avmenc, "--codec=av2", "--webm", "--good", "--cpu-used=6",
             "--passes=1", "--lag-in-frames=0", "--kf-max-dist=8",
             "--end-usage=q", "--cq-level=45", "--threads=8",
             "-o", path, y4m],
            check=True)
    finally:
        os.remove(y4m)


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
    build_av1(os.path.join(MP4, "av1.mp4"))
    build_av1_grain(os.path.join(MP4, "av1_grain.mp4"))
    build_mkv(os.path.join(MKV, "av.mkv"))
    build_pcm_float_mkv(os.path.join(MKV, "pcm_f32.mkv"))
    build_webm(os.path.join(MKV, "vp9.webm"), "libvpx-vp9")
    build_webm(os.path.join(MKV, "vp8.webm"), "libvpx")
    build_av2(os.path.join(MKV, "av2.webm"))
    for directory, name in ((MP4, "av.mp4"), (MP4, "av_bt2020.mp4"), (MP4, "av1.mp4"),
                            (MP4, "av1_grain.mp4"), (MKV, "av.mkv"),
                            (MKV, "pcm_f32.mkv"), (MKV, "vp9.webm"),
                            (MKV, "vp8.webm"), (MKV, "av2.webm")):
        p = os.path.join(directory, name)
        print("%-16s %d bytes" % (name, os.path.getsize(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

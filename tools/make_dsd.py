#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write DSF and DSDIFF files, and check that their bits came back out as DoP.

**Nothing encodes DSD.** FFmpeg decodes DSF and DSDIFF and cannot write either;
no other tool to hand can; there is no reference encoder the way there is for
FLAC and ALAC. So the corpus for this format is made here, by the modulator the
format is defined by -- a second-order sigma-delta loop turning a sine into
one-bit samples at 2.8224 MHz -- and the two small files it writes into
tests/data/dsd are committed, for the same reason the AAC tables are: a build
must not need this script, and a reader must be able to check where the bytes
came from.

    python tools/make_dsd.py tests/data/dsd            # write the corpus
    python tools/make_dsd.py tests/data/dsd --check    # and verify a DoP dump

**The check is the one this format rests on.** DoP loses nothing -- two DSD
bytes ride in each 24-bit frame under an alternating 0x05/0xFA marker -- so the
DSD in the file must be exactly the DSD in the output, and this takes the
markers off and compares. It is the same kind of check FLAC and ALAC get for
free by being lossless, arranged by hand because DSD has no PCM to be equal to.
"""

import math
import os
import struct
import sys

DSD64 = 2_822_400


def modulate(seconds, rate, freq, amplitude=0.4):
    """A second-order sigma-delta loop: a sine in, one bit per sample out."""
    n = int(seconds * rate)
    bits = bytearray(n)
    i1 = 0.0
    i2 = 0.0
    y = 1.0
    step = 2.0 * math.pi * freq / rate
    for i in range(n):
        x = amplitude * math.sin(step * i)
        i1 += x - y
        i2 += i1 - y
        y = 1.0 if i2 >= 0.0 else -1.0
        bits[i] = 1 if y > 0 else 0
        # Keep the integrators from running away, which is what a real
        # modulator's clipping does and what stops this producing a constant.
        i1 = max(-2.0, min(2.0, i1))
        i2 = max(-2.0, min(2.0, i2))
    return bits


def to_bytes_msb(bits):
    """Eight bits to a byte, earliest sample in the most significant bit."""
    out = bytearray(len(bits) // 8)
    for i in range(len(out)):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[i * 8 + j]
        out[i] = b
    return bytes(out)


REVERSED = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


def write_dsf(path, channels_bytes, rate, lsb_first=True, block=4096):
    """Sony DSF: little-endian header, channels blocked, usually LSB first."""
    channels = len(channels_bytes)
    per_channel = len(channels_bytes[0])
    blocks = (per_channel + block - 1) // block
    stored = [bytes(REVERSED[b] for b in c) if lsb_first else c for c in channels_bytes]

    audio = bytearray()
    for g in range(blocks):
        for c in stored:
            chunk = c[g * block:(g + 1) * block]
            # The last block group is zero-padded, which is the format's own
            # rule and the reason a DSF's length comes from its sample count
            # rather than from the size of its data chunk.
            audio += chunk + bytes(block - len(chunk))

    f = bytearray()
    f += b"DSD "
    f += struct.pack("<Q", 28)
    f += struct.pack("<Q", 92 + len(audio))
    f += struct.pack("<Q", 0)                    # no metadata chunk
    f += b"fmt "
    f += struct.pack("<Q", 52)
    f += struct.pack("<I", 1)                    # format version
    f += struct.pack("<I", 0)                    # format id: DSD raw
    f += struct.pack("<I", {1: 1, 2: 2, 6: 7}.get(channels, channels))
    f += struct.pack("<I", channels)
    f += struct.pack("<I", rate)
    f += struct.pack("<I", 1 if lsb_first else 8)
    f += struct.pack("<Q", per_channel * 8)      # DSD samples, so bits
    f += struct.pack("<I", block)
    f += struct.pack("<I", 0)                    # reserved
    f += b"data"
    f += struct.pack("<Q", 12 + len(audio))
    f += audio
    with open(path, "wb") as out:
        out.write(f)
    return per_channel


def write_dff(path, channels_bytes, rate):
    """Philips DSDIFF: big-endian IFF, channels interleaved, MSB first."""
    channels = len(channels_bytes)
    per_channel = len(channels_bytes[0])

    audio = bytearray()
    for i in range(per_channel):
        for c in channels_bytes:
            audio.append(c[i])

    ids = [b"SLFT", b"SRGT", b"C   ", b"LFE ", b"LS  ", b"RS  "][:channels]
    props = bytearray(b"SND ")
    props += b"FS  " + struct.pack(">Q", 4) + struct.pack(">I", rate)
    props += b"CHNL" + struct.pack(">Q", 2 + 4 * channels) + struct.pack(">H", channels)
    for i in ids:
        props += i
    props += b"CMPR" + struct.pack(">Q", 19) + b"DSD " + bytes([14]) + b"not compressed"

    body = bytearray(b"DSD ")                     # the form type, not a chunk
    body += b"FVER" + struct.pack(">Q", 4) + struct.pack(">I", 0x01050000)
    body += b"PROP" + struct.pack(">Q", len(props)) + props
    if len(props) & 1:
        body += b"\0"                             # chunks are padded to even
    body += b"DSD " + struct.pack(">Q", len(audio)) + audio

    with open(path, "wb") as out:
        out.write(bytearray(b"FRM8") + struct.pack(">Q", len(body)) + body)
    return per_channel


def check_dop(dop_path, channels_bytes, frames_expected):
    """Every DSD bit, in order, under the right marker. Returns what was wrong."""
    with open(dop_path, "rb") as f:
        raw = f.read()
    channels = len(channels_bytes)
    frame = 3 * channels
    frames = len(raw) // frame
    problems = []
    if frames != frames_expected:
        problems.append(f"{frames} frames out, {frames_expected} expected")

    got = [bytearray() for _ in range(channels)]
    for i in range(frames):
        want_marker = 0x05 if i % 2 == 0 else 0xFA
        for c in range(channels):
            at = i * frame + c * 3
            lo, hi, marker = raw[at], raw[at + 1], raw[at + 2]
            if marker != want_marker and len(problems) < 4:
                problems.append(f"frame {i} channel {c}: marker {marker:#04x}, "
                                f"expected {want_marker:#04x}")
            got[c].append(hi)   # the earlier DSD byte is the more significant
            got[c].append(lo)
    for c in range(channels):
        want = channels_bytes[c][:len(got[c])]
        if bytes(got[c]) != want:
            first = next((i for i in range(len(want)) if got[c][i] != want[i]), None)
            problems.append(f"channel {c}: the DSD differs, first at byte {first}")
    return problems


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    check = "--check" in sys.argv

    # Small on purpose: this is a corpus a format matrix shows to every decoder,
    # not a listening test. Two wrappers of the same audio, so a row that
    # differs between them is a bug in one of the two readers.
    stereo = [to_bytes_msb(modulate(0.02, DSD64, 440)),
              to_bytes_msb(modulate(0.02, DSD64, 660))]
    per_channel = write_dsf(f"{out}/stereo_dsd64.dsf", stereo, DSD64, lsb_first=True)
    write_dff(f"{out}/stereo_dsd64.dff", stereo, DSD64)
    frames = per_channel // 2
    for name in ("stereo_dsd64.dsf", "stereo_dsd64.dff"):
        print(f"{name:18s} 2ch  DSD64  {per_channel} bytes/ch  {frames} DoP frames")

    if not check:
        return
    bad = 0
    for name in ("stereo_dsd64.dsf", "stereo_dsd64.dff"):
        dump = f"{out}/{name}.dop"
        if not os.path.exists(dump):
            print(f"{name}: nothing to check. Write one with\n"
                  f"    mediaperch-probe decode --file {out}/{name} --raw {dump}")
            continue
        problems = check_dop(dump, stereo, frames)
        print(f"{name}: " + ("every DSD bit, in order, under the right marker"
                             if not problems else "; ".join(problems[:3])))
        bad += len(problems)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

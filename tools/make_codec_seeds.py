#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the seed inputs of the codec fuzzers from the demuxer fuzzers' seed files.

fuzz/codec_fuzzer.cpp reads its input as what a demuxer hands a codec -- one
byte choosing among the codecs the module names, two bytes of configuration
length and the configuration, then packets, each two bytes of length and the
bytes -- so a real file has to be taken apart into that before a codec fuzzer
can start from it. This does it for the files already in fuzz/corpus/: a FLAC
file's STREAMINFO and frames, an Ogg file's headers and packets (Opus, Vorbis),
an MPEG audio file's frames and a WAV file's samples, into
fuzz/corpus/codec_<module>/. The output is committed, as the inputs are, and
comes from them alone: a build does not need this script, and a reader can see
where every byte came from.

    python tools/make_codec_seeds.py            # from the repository's root
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "fuzz", "corpus")


def seed(selector, config, packets):
    """The input, and a zero after it: one byte no packet takes, which the
    FLAC fuzzer reads as "fail no allocation"."""
    out = bytearray([selector])
    out += struct.pack("<H", len(config))
    out += config
    for packet in packets:
        packet = packet[:0xFFFF]
        out += struct.pack("<H", len(packet))
        out += packet
    out.append(0)
    return bytes(out)


def flac(data):
    """STREAMINFO, and the frames cut at their sync codes."""
    if data[:4] != b"fLaC":
        return None
    at = 4
    streaminfo = None
    while at + 4 <= len(data):
        last = data[at] & 0x80
        kind = data[at] & 0x7F
        length = int.from_bytes(data[at + 1:at + 4], "big")
        if kind == 0:
            streaminfo = data[at + 4:at + 4 + length]
        at += 4 + length
        if last:
            break
    if streaminfo is None or len(streaminfo) != 34:
        return None
    starts = [i for i in range(at, len(data) - 1)
              if data[i] == 0xFF and data[i + 1] in (0xF8, 0xF9)]
    frames = [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]
    return streaminfo, frames


def ogg_packets(data):
    """The packets of every page, joined across pages where a lacing value of
    255 says a packet goes on."""
    packets = []
    current = b""
    at = 0
    while at + 27 <= len(data) and data[at:at + 4] == b"OggS":
        segments = data[at + 26]
        table = data[at + 27:at + 27 + segments]
        body = at + 27 + segments
        for lacing in table:
            current += data[body:body + lacing]
            body += lacing
            if lacing < 255:
                packets.append(current)
                current = b""
        at = body
    return packets


def vorbis_config(headers):
    """The configuration module.h gives MP_CODEC_VORBIS: the three header
    packets, each preceded by its length as a 32-bit little-endian integer."""
    out = bytearray()
    for header in headers:
        out += struct.pack("<I", len(header))
        out += header
    return bytes(out)


def chunks(data, size):
    return [data[i:i + size] for i in range(0, len(data), size)]


def wav_samples(data):
    """The bytes of a WAV file's data chunk."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    at = 12
    while at + 8 <= len(data):
        kind = data[at:at + 4]
        size = int.from_bytes(data[at + 4:at + 8], "little")
        if kind == b"data":
            return data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    return None


def write(module, name, contents):
    directory = os.path.join(CORPUS, "codec_" + module)
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name), "wb") as f:
        f.write(contents)
    print(f"codec_{module}/{name}: {len(contents)} bytes")


def read(*parts):
    with open(os.path.join(CORPUS, *parts), "rb") as f:
        return f.read()


def main():
    for name in sorted(os.listdir(os.path.join(CORPUS, "flac"))):
        taken = flac(read("flac", name))
        if taken:
            streaminfo, frames = taken
            write("flac", os.path.splitext(name)[0], seed(0, streaminfo, frames))

    packets = ogg_packets(read("ogg", "seed_opus.opus"))
    if packets and packets[0][:8] == b"OpusHead":
        # The head is the configuration and the tags are nothing a decoder reads.
        write("opus", "seed_opus", seed(0, packets[0], packets[2:]))

    packets = ogg_packets(read("ogg", "seed_vorbis.ogg"))
    if len(packets) >= 3 and packets[0][1:7] == b"vorbis":
        write("vorbis", "seed_vorbis", seed(0, vorbis_config(packets[:3]), packets[3:]))

    # MPEG audio: the module names MP1, MP2 and MP3 in that order, so 2 is MP3.
    # libmpg123 reads a stream, so frames cut anywhere still decode.
    for name in sorted(os.listdir(os.path.join(CORPUS, "mp3"))):
        write("mpa", name, seed(2, b"", chunks(read("mp3", name), 417)))

    for name in sorted(os.listdir(os.path.join(CORPUS, "wav"))):
        samples = wav_samples(read("wav", name))
        if samples:
            write("pcm", os.path.splitext(name)[0], seed(0, b"", chunks(samples, 1024)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

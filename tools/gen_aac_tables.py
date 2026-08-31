#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate modules/decode_aac/aac_tables.cpp from FFmpeg's libavcodec/aactab.c.

The tables this emits are constants of ISO/IEC 14496-3: the eleven spectrum
Huffman codebooks, the scalefactor codebook, the scalefactor band offsets for
every sample rate, and the TNS band limits. They are the same numbers in every
AAC implementation that exists, because the standard fixes them -- there is no
version of this decoder that could have different ones and still decode AAC.

They are transcribed rather than typed, and the transcription is a script rather
than an afternoon, so that the provenance is checkable and the result is
reproducible. FFmpeg is the source because libavcodec is LGPL-2.1-or-later,
which this project's GPL-3.0-or-later can take, and because its tables are in
the standard's own (length, codeword) form rather than in some decoder's
internal shape.

The output is committed, so no build ever needs the network or this script.

    python tools/gen_aac_tables.py path/to/ffmpeg/libavcodec/aactab.c

Every codebook emitted is checked against the Kraft equality -- the codeword
lengths of a complete prefix code satisfy sum(2^-len) == 1 -- which catches a
mis-parsed or truncated table immediately rather than as wrong audio later.
"""

import re
import sys
from fractions import Fraction

WANT_ARRAYS = (
    [f"codes{i}" for i in range(1, 12)]
    + [f"bits{i}" for i in range(1, 12)]
    + [
        "ff_aac_spectral_sizes",
        "ff_aac_scalefactor_code",
        "ff_aac_scalefactor_bits",
        "ff_aac_num_swb_1024",
        "ff_aac_num_swb_128",
        "ff_tns_max_bands_1024",
        "ff_tns_max_bands_128",
    ]
)

# The scalefactor band offsets, one array per sample-rate group.
SWB_RE = re.compile(r"swb_offset_(1024|128)_(\d+)")


def parse_arrays(text):
    """Every `... name[...] = { ... };` in the file, as name -> [ints]."""
    out = {}
    pattern = re.compile(
        r"(?:static\s+)?const\s+\w+\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        name, body = match.group(1), match.group(2)
        if "{" in body:  # a table of tables; not one of ours
            continue
        values = []
        for token in body.replace("\n", " ").split(","):
            token = token.split("//")[0].strip()
            if not token:
                continue
            token = re.sub(r"/\*.*?\*/", "", token).strip()
            if not token:
                continue
            try:
                values.append(int(token, 0))
            except ValueError:
                values = None
                break
        if values:
            out[name] = values
    return out


def parse_pointer_table(text, name):
    """`const T * const name[] = { a, b, c };` as a list of array names."""
    match = re.search(
        r"const\s+\w+\s*\*\s*const\s+" + re.escape(name) + r"\s*\[\]\s*=\s*\{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not match:
        raise SystemExit(f"could not find the pointer table {name}")
    body = re.sub(r"/\*.*?\*/", " ", match.group(1), flags=re.DOTALL)
    body = re.sub(r"//[^\n]*", " ", body)
    return [t.strip() for t in body.replace("\n", " ").split(",") if t.strip()]


def kraft_ok(lengths):
    """A complete prefix code satisfies sum(2^-len) == 1 exactly."""
    return sum(Fraction(1, 1 << n) for n in lengths) == 1


def emit(values, per_line=16, width=5):
    out, line = [], "   "
    for value in values:
        piece = f" {value:{width}},"
        if len(line) + len(piece) > 96:
            out.append(line)
            line = "   "
        line += piece
    if line.strip():
        out.append(line)
    return "\n".join(out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    arrays = parse_arrays(text)

    missing = [n for n in WANT_ARRAYS if n not in arrays]
    if missing:
        raise SystemExit(f"missing from the source: {', '.join(missing)}")

    # Check every spectrum codebook before writing a single byte of output.
    for book in range(1, 12):
        lengths = arrays[f"bits{book}"]
        codes = arrays[f"codes{book}"]
        size = arrays["ff_aac_spectral_sizes"][book - 1]
        if len(lengths) != size or len(codes) != size:
            raise SystemExit(
                f"codebook {book}: {len(lengths)} lengths and {len(codes)} codes "
                f"for a declared size of {size}"
            )
        if not kraft_ok(lengths):
            raise SystemExit(f"codebook {book} is not a complete prefix code")
        print(f"  codebook {book:2d}: {size:3d} entries, Kraft equality holds")

    if not kraft_ok(arrays["ff_aac_scalefactor_bits"]):
        raise SystemExit("the scalefactor codebook is not a complete prefix code")
    print(f"  scalefactor: {len(arrays['ff_aac_scalefactor_bits'])} entries, Kraft holds")

    swb_1024 = parse_pointer_table(text, "ff_swb_offset_1024")
    swb_128 = parse_pointer_table(text, "ff_swb_offset_128")
    if len(swb_1024) != 13 or len(swb_128) != 13:
        raise SystemExit("expected 13 sample-rate slots in each swb table")

    lines = []
    w = lines.append
    w("// SPDX-License-Identifier: GPL-3.0-or-later")
    w("//")
    w("// GENERATED by tools/gen_aac_tables.py -- do not edit.")
    w("//")
    w("// These are constants of ISO/IEC 14496-3, transcribed from FFmpeg's")
    w("// libavcodec/aactab.c, which is LGPL-2.1-or-later. Every AAC decoder that")
    w("// exists contains the same numbers; the standard fixes them.")
    w("//")
    w("// Every codebook here satisfies the Kraft equality, checked by the generator")
    w("// before this file was written.")
    w("")
    w('#include "aac_tables.hpp"')
    w("")
    w("namespace mp::aac {")
    w("")

    for book in range(1, 12):
        size = arrays["ff_aac_spectral_sizes"][book - 1]
        w(f"const std::uint16_t k_codes{book}[{size}] = {{")
        w(emit(arrays[f"codes{book}"]))
        w("};")
        w(f"const std::uint8_t k_bits{book}[{size}] = {{")
        w(emit(arrays[f"bits{book}"], width=3))
        w("};")
        w("")

    w("const std::uint16_t* const k_spectral_codes[11] = {")
    w("    " + ", ".join(f"k_codes{i}" for i in range(1, 12)) + ",")
    w("};")
    w("const std::uint8_t* const k_spectral_bits[11] = {")
    w("    " + ", ".join(f"k_bits{i}" for i in range(1, 12)) + ",")
    w("};")
    w("const std::uint16_t k_spectral_sizes[11] = {")
    w("    " + ", ".join(str(v) for v in arrays["ff_aac_spectral_sizes"]) + ",")
    w("};")
    w("")

    w(f"const std::uint32_t k_scalefactor_code[{len(arrays['ff_aac_scalefactor_code'])}] = {{")
    w(emit(arrays["ff_aac_scalefactor_code"], width=8))
    w("};")
    w(f"const std::uint8_t k_scalefactor_bits[{len(arrays['ff_aac_scalefactor_bits'])}] = {{")
    w(emit(arrays["ff_aac_scalefactor_bits"], width=3))
    w("};")
    w("")

    for group, names in (("1024", swb_1024), ("128", swb_128)):
        seen = []
        for name in names:
            if name not in arrays:
                raise SystemExit(f"{name} referenced but not defined")
            if name not in seen:
                seen.append(name)
        for name in seen:
            values = arrays[name]
            w(f"static const std::uint16_t k_{name}[{len(values)}] = {{")
            w(emit(values, width=5))
            w("};")
        w(f"const std::uint16_t* const k_swb_offset_{group}[13] = {{")
        w("    " + ", ".join(f"k_{n}" for n in names) + ",")
        w("};")
        w("")

    for name, out_name in (
        ("ff_aac_num_swb_1024", "k_num_swb_1024"),
        ("ff_aac_num_swb_128", "k_num_swb_128"),
        ("ff_tns_max_bands_1024", "k_tns_max_bands_1024"),
        ("ff_tns_max_bands_128", "k_tns_max_bands_128"),
    ):
        values = arrays[name]
        w(f"const std::uint8_t {out_name}[{len(values)}] = {{")
        w(emit(values, width=3))
        w("};")
        w("")

    w("} // namespace mp::aac")
    w("")

    with open("modules/decode_aac/aac_tables.cpp", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote modules/decode_aac/aac_tables.cpp")


if __name__ == "__main__":
    main()

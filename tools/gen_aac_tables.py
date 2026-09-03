#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate modules/codec/aac/decoder/src/tables.rs, the constant tables of AAC.

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

**It also reads its own earlier output.** The decoder was C++ before it was
Rust, and the C++ tables file this script wrote then -- `aac_tables.cpp`, in
git history -- is the same transcription in a different syntax. The Rust file
was first produced from that file, because FFmpeg's source was not to hand and
a transcription of a checked transcription is still checked:

    python tools/gen_aac_tables.py <aac_tables.cpp from history>

Either way every codebook is checked against the Kraft equality -- the codeword
lengths of a complete prefix code satisfy sum(2^-len) == 1 -- which catches a
mis-parsed or truncated table immediately rather than as wrong audio later.
"""

import re
import sys
from fractions import Fraction

OUT = "modules/codec/aac/decoder/src/tables.rs"

# FFmpeg's names for what is wanted, and the names the C++ transcription used.
ALIASES = {
    **{f"codes{i}": f"k_codes{i}" for i in range(1, 12)},
    **{f"bits{i}": f"k_bits{i}" for i in range(1, 12)},
    "ff_aac_spectral_sizes": "k_spectral_sizes",
    "ff_aac_scalefactor_code": "k_scalefactor_code",
    "ff_aac_scalefactor_bits": "k_scalefactor_bits",
    "ff_aac_num_swb_1024": "k_num_swb_1024",
    "ff_aac_num_swb_128": "k_num_swb_128",
    "ff_tns_max_bands_1024": "k_tns_max_bands_1024",
    "ff_tns_max_bands_128": "k_tns_max_bands_128",
    "ff_swb_offset_1024": "k_swb_offset_1024",
    "ff_swb_offset_128": "k_swb_offset_128",
}


def parse_arrays(text):
    """Every `... name[...] = { ... };` in the file, as name -> [ints]."""
    out = {}
    pattern = re.compile(
        r"(?:static\s+)?const\s+[\w:]+\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
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
    """`const T * const name[...] = { a, b, c };` as a list of array names."""
    match = re.search(
        r"const\s+[\w:]+\s*\*\s*const\s+"
        + re.escape(name)
        + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
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


def emit(values, width=5):
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
    raw = parse_arrays(text)

    # Whichever naming the input used, the tables are looked up by FFmpeg's.
    from_cpp = "k_spectral_sizes" in raw
    arrays = {}
    for ff_name, cpp_name in ALIASES.items():
        key = cpp_name if from_cpp else ff_name
        if key in raw:
            arrays[ff_name] = raw[key]
    for name, values in raw.items():
        if name.startswith("k_swb_offset_") or name.startswith("swb_offset_"):
            arrays[name.removeprefix("k_")] = values

    want = [n for n in ALIASES if not n.startswith("ff_swb_offset")]
    missing = [n for n in want if n not in arrays]
    if missing:
        raise SystemExit(f"missing from the source: {', '.join(missing)}")
    source = "the C++ transcription" if from_cpp else "FFmpeg's libavcodec/aactab.c"
    print(f"reading {source}")

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

    def slots(ff_name):
        names = parse_pointer_table(text, ALIASES[ff_name] if from_cpp else ff_name)
        return [n.removeprefix("k_") for n in names]

    swb_1024 = slots("ff_swb_offset_1024")
    swb_128 = slots("ff_swb_offset_128")
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
    w("/// One of the eleven spectrum codebooks, as the standard gives it: a codeword")
    w("/// and its length per index. A decoding structure is built from these at")
    w("/// first use rather than stored, so what ships is the standard's data and not")
    w("/// a particular decoder's idea of how to search it.")
    w("pub struct Codebook {")
    w("    pub codes: &'static [u16],")
    w("    pub bits: &'static [u8],")
    w("}")
    w("")

    for book in range(1, 12):
        size = arrays["ff_aac_spectral_sizes"][book - 1]
        w(f"static CODES{book}: [u16; {size}] = [")
        w(emit(arrays[f"codes{book}"]))
        w("];")
        w(f"static BITS{book}: [u8; {size}] = [")
        w(emit(arrays[f"bits{book}"], width=3))
        w("];")
        w("")

    w("/// Codebooks 1 to 11, so index with `cb - 1`.")
    w("pub static SPECTRAL: [Codebook; 11] = [")
    for book in range(1, 12):
        w(f"    Codebook {{ codes: &CODES{book}, bits: &BITS{book} }},")
    w("];")
    w("")

    n = len(arrays["ff_aac_scalefactor_code"])
    w("/// The scalefactor codebook: 121 entries covering deltas of -60..=60. 32-bit")
    w("/// because it has to be: its longest codeword is nineteen bits.")
    w(f"pub static SCALEFACTOR_CODE: [u32; {n}] = [")
    w(emit(arrays["ff_aac_scalefactor_code"], width=8))
    w("];")
    w(f"pub static SCALEFACTOR_BITS: [u8; {n}] = [")
    w(emit(arrays["ff_aac_scalefactor_bits"], width=3))
    w("];")
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
            w(f"static {name.upper()}: [u16; {len(values)}] = [")
            w(emit(values, width=5))
            w("];")
        which = "long" if group == "1024" else "short"
        w(f"/// Scalefactor band offsets for {which} windows, indexed by the")
        w("/// sample-rate index. Each ends with the window length.")
        w(f"pub static SWB_OFFSET_{group}: [&[u16]; 13] = [")
        w("    " + ", ".join(f"&{n.upper()}" for n in names) + ",")
        w("];")
        w("")

    for name, out_name, doc in (
        ("ff_aac_num_swb_1024", "NUM_SWB_1024", "How many bands each rate has, long windows."),
        ("ff_aac_num_swb_128", "NUM_SWB_128", "How many bands each rate has, short windows."),
        (
            "ff_tns_max_bands_1024",
            "TNS_MAX_BANDS_1024",
            "The highest band TNS may filter, per rate, long windows.",
        ),
        (
            "ff_tns_max_bands_128",
            "TNS_MAX_BANDS_128",
            "The highest band TNS may filter, per rate, short windows.",
        ),
    ):
        values = arrays[name]
        w(f"/// {doc}")
        w(f"pub static {out_name}: [u8; {len(values)}] = [")
        w(emit(values, width=3))
        w("];")
        w("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()

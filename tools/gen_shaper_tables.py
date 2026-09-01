#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transcribe SSRC's noise-shaping coefficients into this tree.

The psychoacoustically weighted shaping curves are measured tables, not
expressions: there is nothing to derive and everything to mistype. So they
arrive the way the AAC Huffman tables arrived -- from a named source, through a
generator that refuses to write a table failing its own checks, with the source
recorded in the output.

Two sources, and they do not share a sign convention -- which is the single
most important thing in this file. SSRC *adds* the filtered error history to the
sample; ReSampler *subtracts* it. The same numbers under the wrong convention
shape the noise into the midband instead of out of it, which is worse than no
shaping at all and sounds like nothing is wrong. ReSampler's are negated on the
way in so that one convention, SSRC's, is the only one at run time.

    Source:  https://github.com/shibatch/ssrc
             src/include/shibatch/shapercoefs.h
             "Noise shaper coefficients for SSRC, written by Naoki Shibata"
    Licence: Boost Software License 1.0, which is GPL-3.0 compatible.

    ATH-weighted -- the absolute threshold of hearing -- which is why these are
    indexed by sample rate as well as intensity: where the ear stops listening
    is a fixed frequency, and where that lands in the spectrum depends on how
    fast the samples go past. A curve is never substituted across rates.

    Source:  https://github.com/jniemann66/ReSampler
             noiseshape.h, (C) Judd Niemann
    Licence: LGPL-2.1, which is GPL-3.0 compatible.

    Published curves -- Lipshitz, Vanderkooy and Wannamaker's "Minimally
    Audible Noise Shaping" and Wannamaker's "Psychoacoustically Optimal Noise
    Shaping" -- plus ReSampler's own. Designed for 44.1 kHz and usable at other
    rates, where the shape stretches with the rate and its notches move off the
    frequencies they were placed at. That is ReSampler's own documented
    position and it is recorded here rather than silently relied on.

Usage:
    python tools/gen_shaper_tables.py shapercoefs.h [noiseshape.h]
"""
import io
import re
import sys

ENTRY = re.compile(
    r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*\{([^}]*)\}[^}]*\}',
    re.S)

# Just the head of an entry, used only to count how many there are. Two of them
# carry a trailing comment between the closing braces, which the full pattern
# did not allow -- so it parsed 65 of 67 and said nothing. A generator that
# silently writes most of a table is worse than one that crashes, which is what
# this exists to make impossible.
HEAD = re.compile(r'\{\s*\d+\s*,\s*\d+\s*,\s*"[^"]*"\s*,\s*\d+\s*,')

# ReSampler: `const double wan9[9] = { ... };`, one array per curve.
ARRAY = re.compile(r'const\s+double\s+(\w+)\s*\[\s*(\d+)\s*\]\s*=\s*\{([^}]*)\}\s*;',
                   re.S)

# Which of ReSampler's arrays are FIR error-feedback curves, and what to call
# them. `classic` is left out on purpose: it is a cascaded biquad in ReSampler
# rather than an FIR, so its coefficients are not taps and transcribing them
# here would produce a filter that is not the one the name means.
RESAMPLER_CURVES = [
    ('noiseShaperPassThrough', 'flat', 'flat, with error feedback: violet TPDF noise'),
    ('modew44', 'modified-e', 'modified E-weighted, notch at 3-4 kHz'),
    ('lips44', 'lipshitz', 'Lipshitz E-weighted, notches near 4k and 12k'),
    ('impew44', 'improved-e', 'improved E-weighted, 9 taps'),
    ('wan3', 'wannamaker3', 'Wannamaker 3-tap f-weighted, notch near 4 kHz'),
    ('wan9', 'wannamaker9', "Wannamaker 9-tap, SoX's f-weighted"),
    ('wan24', 'wannamaker24', 'Wannamaker 24-tap, notches near 3.5k and 12k'),
    ('std_44', 'standard', "ReSampler's default: notches at 3150 and 11250 Hz"),
    ('high28', 'high28', 'notches at 3150 and 11250 Hz, 28 dB high shelf'),
    ('high30', 'high30', 'notches at 3150 and 11250 Hz, 30 dB high shelf'),
    ('high32', 'high32', 'notches at 3150 and 11250 Hz, 32 dB high shelf'),
    ('blue', 'blue', 'blue noise: 3 dB per octave rise'),
]


def parse_resampler(text):
    """ReSampler's arrays, negated into SSRC's convention."""
    arrays = {name: (int(n), numbers(body))
              for name, n, body in ARRAY.findall(text)}
    out = []
    for symbol, name, description in RESAMPLER_CURVES:
        if symbol not in arrays:
            raise SystemExit('noiseshape.h has no array called %s' % symbol)
        declared, coefs = arrays[symbol]
        if len(coefs) != declared:
            raise SystemExit('%s declares %d coefficients and has %d'
                             % (symbol, declared, len(coefs)))
        for c in coefs:
            if not (-K_BOUND < c < K_BOUND):
                raise SystemExit('%s has a coefficient of %g' % (symbol, c))
        # Negated: ReSampler subtracts the filtered history where SSRC adds it.
        out.append((0, 0, name + ': ' + description, [-c for c in coefs]))
    return out

# A coefficient far outside this is not a shaper, it is a transcription error.
# SSRC's own curves sit well inside it; the check is here to catch a parse that
# ran off the end of an entry, which is the failure mode that produces numbers
# that look almost right.
K_BOUND = 16.0


COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
NUMBER = re.compile(r'-?\d+\.?\d*(?:[eE][-+]?\d+)?')


def numbers(body):
    """Every number in an initialiser, and nothing from the comments in it.

    `const double modew44[9] = { // Modified E-weighted (appendix: 2)` has a 2
    in the comment, and reading it as a coefficient made a 9-tap filter into a
    10-tap one. The generator caught it because the declared length disagreed;
    it would not have caught a comment whose digits happened to land on the
    right count.
    """
    return [float(x) for x in NUMBER.findall(COMMENT.sub(' ', body))]


def parse(text):
    out = []
    expected = len(HEAD.findall(text))
    for rate, intensity, name, length, body in ENTRY.findall(text):
        coefs = numbers(body)
        rate, intensity, length = int(rate), int(intensity), int(length)
        # The arrays are a fixed size and `len` says how much of one is real;
        # the rest is zero padding. Taking the whole array would hand the filter
        # taps that are not part of the curve, so the pad is checked and
        # dropped rather than trusted.
        if len(coefs) < length:
            raise SystemExit(
                'entry %d/%d "%s" declares %d coefficients and has %d'
                % (rate, intensity, name, length, len(coefs)))
        if any(c != 0.0 for c in coefs[length:]):
            raise SystemExit(
                'entry %d/%d "%s" has a non-zero value past its declared length'
                % (rate, intensity, name))
        coefs = coefs[:length]
        for c in coefs:
            if not (-K_BOUND < c < K_BOUND):
                raise SystemExit('entry %d/%d "%s" has a coefficient of %g'
                                 % (rate, intensity, name, c))
        out.append((rate, intensity, name, coefs))

    if len(out) != expected:
        raise SystemExit('the file has %d entries and only %d parsed' %
                         (expected, len(out)))
    return out


HEADER = '''// SPDX-License-Identifier: GPL-3.0-or-later
//
// GENERATED by tools/gen_shaper_tables.py. Do not edit.
//
// Noise-shaping coefficients from SSRC, https://github.com/shibatch/ssrc,
// written by Naoki Shibata and licensed under the Boost Software License 1.0.
// They are transcribed rather than derived: these are measured curves weighted
// by the absolute threshold of hearing, which is also why they are indexed by
// sample rate -- where the ear stops listening is a fixed frequency, and where
// that sits in the spectrum depends on how fast the samples go past.
//
// %d curves, %d sample rates, %d coefficients in total.

#include "mediaperch/shaper_tables.hpp"

namespace mp {
namespace {

const double k_coefficients[] = {
'''


def emit(entries, path):
    flat = []
    spans = []
    for _rate, _intensity, _name, coefs in entries:
        spans.append((len(flat), len(coefs)))
        flat.extend(coefs)

    rates = sorted({e[0] for e in entries})
    body = io.StringIO()
    body.write(HEADER % (len(entries), len(rates), len(flat)))
    for i in range(0, len(flat), 3):
        body.write('    ' + ' '.join('%.20g,' % v for v in flat[i:i + 3]) + '\n')
    body.write('};\n\nconst ShaperCurve k_curves[] = {\n')
    for (rate, intensity, name, coefs), (at, n) in zip(entries, spans):
        body.write('    {%d, %d, "%s", k_coefficients + %d, %d},\n'
                   % (rate, intensity, name.replace('"', ''), at, n))
    body.write('};\n\n} // namespace\n\n')
    body.write('''std::span<const ShaperCurve> shaper_curves() noexcept
{
    return std::span<const ShaperCurve>{k_curves, std::size(k_curves)};
}

} // namespace mp
''')
    io.open(path, 'w', encoding='utf-8', newline='\n').write(body.getvalue())


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    entries = parse(io.open(sys.argv[1], encoding='utf-8', errors='replace').read())
    if not entries:
        raise SystemExit('no coefficient tables found in %s' % sys.argv[1])

    named = []
    if len(sys.argv) == 3:
        named = parse_resampler(
            io.open(sys.argv[2], encoding='utf-8', errors='replace').read())
        if len(named) != len(RESAMPLER_CURVES):
            raise SystemExit('expected %d named curves and built %d'
                             % (len(RESAMPLER_CURVES), len(named)))
    entries = entries + named

    # Every curve must name a length that matches, which parse() checked, and
    # the set must cover the rates a decoder in this tree can produce. 44.1 and
    # 48 are the ones that matter; the check is that they are there at all.
    rates = {e[0] for e in entries}
    for needed in (44100, 48000):
        if needed not in rates:
            raise SystemExit('no curves for %d Hz' % needed)

    out = 'src/core/mediaperch/shaper_tables.cpp'
    emit(entries, out)
    print('%s: %d rate-indexed curves over %d rates, %d named curves, '
          '%d coefficients'
          % (out, len(entries) - len(named), len(rates) - (1 if named else 0),
             len(named), sum(len(e[3]) for e in entries)))


if __name__ == '__main__':
    main()

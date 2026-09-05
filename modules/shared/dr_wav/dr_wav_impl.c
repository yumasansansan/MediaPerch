/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dr_wav's implementation, in a translation unit of its own and a target of its
 * own.
 *
 * It is a large single-header C library and it is not ours. It used to be
 * compiled twice -- once for demux_wav and once for the impulse reader -- with
 * this project's warnings switched off per source file. Now it is compiled
 * once, into a target marked MEDIAPERCH_EXTERNAL, which is the only thing that
 * keeps the tree's warning set off it: no flag is set on its behalf.
 *
 * demux_wav.cpp defines DR_WAV_NO_CONVERSION_API before including the header,
 * so the reader still cannot see the conversion API even though this
 * implementation carries it -- the impulse reader is the one that wants it.
 *
 * dr_wav is public domain / MIT-0. See the licence statement at the end of the
 * header.
 */

#define DR_WAV_IMPLEMENTATION

/* dr_wav refuses any file above 384 kHz. That is its own sanity ceiling against
 * garbage headers, not a WAV limit -- the field is 32 bits wide -- and it
 * turned a perfectly good 768 kHz file into "unsupported by this module".
 * Raised to 128x48k, which is past every rate that exists (768 kHz PCM,
 * 2.8 MHz for DSD1024 over DoP) while still bounding what a malformed header
 * can claim. Here, because this is the translation unit that checks it. */
#define DRWAV_MAX_SAMPLE_RATE 6144000

#include <dr_wav.h>

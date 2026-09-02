/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dr_wav's implementation, in a translation unit of its own.
 *
 * It is a large single-header C library and it is not ours, so it is compiled as
 * C with this project's warnings turned off, and nothing else lives in here.
 * `demux_wav.cpp` includes the same header as declarations only and is compiled
 * with the full set.
 *
 * dr_wav is public domain / MIT-0. See the licence statement at the end of the
 * header.
 */

#define DR_WAV_IMPLEMENTATION

/* Nothing here encodes, and a container reader has no business converting: the
 * conversion API is what would let it, so it is not compiled in. */
#define DR_WAV_NO_CONVERSION_API

#include <dr_wav.h>

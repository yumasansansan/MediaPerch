/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dr_mp3's implementation, in a translation unit of its own -- the same
 * arrangement decode_native uses for dr_wav and dr_flac, and for the same
 * reason: it is a large single-header C library, it is not ours, and it is
 * compiled as C with this project's warnings off.
 *
 * DR_MP3_FLOAT_OUTPUT is set on the target rather than here, so that this file
 * and codec_mp3.cpp cannot disagree about the size of drmp3's frame buffer.
 *
 * dr_mp3 is public domain / MIT-0; see the licence statements at the end of the
 * header.
 */

#define DR_MP3_IMPLEMENTATION

#include <dr_mp3.h>

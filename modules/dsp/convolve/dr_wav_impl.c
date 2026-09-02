/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dr_wav, once, for this module. Separate from decode_native's copy because
 * that one is built with the conversion API switched off -- it hands a decoder
 * the file's own bytes and converts nothing. An impulse response is the other
 * case: whatever it was measured in, what is wanted is samples.
 */
#define DR_WAV_IMPLEMENTATION
#define DRWAV_MAX_SAMPLE_RATE 6144000
#include <dr_wav.h>

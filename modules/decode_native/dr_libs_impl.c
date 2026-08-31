/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The dr_libs implementations, in a translation unit of their own.
 *
 * They are large single-header C libraries and they are not ours, so they are
 * compiled as C with this project's warnings turned off, and nothing else lives
 * in here. `decode_native.cpp` includes the same headers as declarations only
 * and is compiled with the full set.
 *
 * dr_wav and dr_flac are public domain / MIT-0. See the licence statements at
 * the end of each header.
 */

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION

/* Nothing here encodes, and nothing here needs the metadata-reading paths yet.
 * Turning them off keeps the module small and the attack surface smaller. */
#define DR_WAV_NO_CONVERSION_API
#define DR_FLAC_NO_OGG

#include <dr_wav.h>

#include <dr_flac.h>

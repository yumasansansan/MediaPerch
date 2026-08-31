/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file exists to be compiled as C.
 *
 * include/mediaperch/module.h is the only place two languages meet, and a header
 * that has only ever been through a C++ compiler has not been tested for the job
 * it was written for. Everything interesting happens at compile time: the
 * MP_STATIC_ASSERT block in the header fires here under C rules, with C's
 * alignment and C's enum sizes, and disagreement with the C++ side is a build
 * failure rather than a runtime surprise on somebody else's machine.
 */
#include <mediaperch/module.h>

/* Reachable from C++ (see abi_test.cpp) so the linker also has to agree that the
 * two translation units produced compatible objects. */
unsigned int mp_abi_probe_from_c(void);

unsigned int mp_abi_probe_from_c(void)
{
    /* Exercise the value macros too: a macro that does not expand in C is a
     * macro that breaks a third-party module and nothing else. */
    MpFormat format;
    format.sample_rate = 44100u;
    format.channels = 2u;
    format.channel_mask = MP_SPEAKER_FRONT_LEFT | MP_SPEAKER_FRONT_RIGHT;
    format.sample_type = MP_SAMPLE_S16;
    format.encoding = MP_ENCODING_PCM;
    format.valid_bits = 0u;
    format.reserved[0] = 0u;
    format.reserved[1] = 0u;

    if (format.sample_rate != 44100u) {
        return 0u;
    }
    if (MP_MAKE_VERSION(0, 1, 0) == 0u) {
        return 0u;
    }
    return MP_ABI_VERSION;
}

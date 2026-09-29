// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that reads a value which was never written, so that a test can
// show MemorySanitizer is really looking. It says nothing when it finds
// nothing, and silence cannot tell "nothing was read uninitialised" from "the
// check never ran": this is the read that has to be reported for the silence of
// every fuzz run to mean anything, as leak_canary.c and race_canary.c are for
// theirs. ADLplug-Next keeps the same program for the same reason.
//
// The memory comes from malloc rather than being a local of this function,
// since a local read before it is written is something the compiler itself
// warns about, and this tree builds with its warnings as errors. It is read
// through a volatile pointer, because an optimised build is free to remove a
// malloc and the free that answers it when nothing else comes of them, and
// then there is no read left to report. Nothing about the read is a matter of
// timing: every run reads the same value that was never written, and the
// sanitizer says where it was read and where the memory came from.

#include <stdlib.h>

int main(void)
{
    int *allocated = malloc(sizeof *allocated);
    if (allocated == nullptr) {
        return EXIT_FAILURE;
    }
    // Never written, on purpose, and read where the build cannot do away with it.
    volatile const int *value = allocated;
    const int read = *value;
    free(allocated);
    // Read rather than only copied: the value decides something.
    if (read == 42) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that leaks on purpose, so that a test can show LeakSanitizer is
// really looking. LeakSanitizer says nothing when it finds nothing, and silence
// cannot tell "no leaks" from "the check never ran": this is the leak that has
// to be reported for the silence of every other test and fuzz run to mean
// anything. ADLplug-Next keeps the same program for the same reason.
//
// The block is dropped in a function of its own, which the compiler may not
// inline, so that no register or stack slot of main still holds its address
// when the check runs at exit.

#include <stdlib.h>
#include <string.h>

static void *volatile lost;

[[gnu::noinline]] static void lose(void)
{
    lost = malloc(64);
    memset(lost, 0x55, 64);
    lost = nullptr;
}

int main(void)
{
    lose();
    return 0;
}

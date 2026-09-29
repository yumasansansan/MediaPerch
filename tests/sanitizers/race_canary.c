// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that races on purpose, so that a test can show ThreadSanitizer is
// really looking. It says nothing when it finds nothing, and silence cannot
// tell "no races" from "the check never ran": this is the race that has to be
// reported for the silence of every other test and fuzz run to mean anything.
// ADLplug-Next keeps the same program for the same reason.
//
// Two threads write the same object with nothing to order the one against the
// other, which is the whole of what the program does. The object is volatile,
// so that an optimised build still has the writes for the sanitizer to see.
//
// **The two threads meet before they write.** Without that, a program this
// short can have one thread write and finish before the other starts, and the
// sanitizer then has two writes that never overlapped in time: the race is in
// the program, and a run of it need not be one the sanitizer names. A test of
// the sanitizer has to fail only when the sanitizer is not looking, so the
// writes are made to overlap rather than left to chance -- both threads wait
// until both have arrived, and only then write, over and over. The waiting is
// ordering, and it orders what came before it; what the threads do afterwards
// is as unordered as it ever was. A mutex and a condition variable make the
// meeting rather than a barrier, since pthread_barrier_* is an option of POSIX
// whose declarations strict C23 does not have.

#include <pthread.h>
#include <stdlib.h>

static volatile int shared;

// How many times each thread writes. One write from each would be enough for
// the sanitizer to compare; a thousand is so that a report does not depend on
// which of the two gets there first.
enum { writes = 1000 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static unsigned waiting;

// Returns when both threads have called it, so that neither writes alone.
static int meet(void)
{
    if (pthread_mutex_lock(&lock) != 0) {
        return 0;
    }
    ++waiting;
    if (waiting == 2 && pthread_cond_broadcast(&arrived) != 0) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    while (waiting < 2) {
        if (pthread_cond_wait(&arrived, &lock) != 0) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
    }
    return pthread_mutex_unlock(&lock) == 0;
}

static void *write_one(void *unused)
{
    (void)unused;
    if (!meet()) {
        return nullptr;
    }
    for (int i = 0; i < writes; ++i) {
        shared = 1;
    }
    return nullptr;
}

int main(void)
{
    pthread_t other;
    if (pthread_create(&other, nullptr, write_one, nullptr) != 0) {
        return EXIT_FAILURE;
    }
    if (!meet()) {
        return EXIT_FAILURE;
    }
    for (int i = 0; i < writes; ++i) {
        shared = 2;
    }
    if (pthread_join(other, nullptr) != 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

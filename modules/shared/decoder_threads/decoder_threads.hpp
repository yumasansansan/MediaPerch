// SPDX-License-Identifier: GPL-3.0-or-later
//
// How many threads a libvpx-family decoder is told it may use.
//
// **Zero does not mean "the library picks" in libvpx, libaom or avm.** Each
// copies `cfg.threads` straight into `pbi->max_threads`, and every
// multithreaded path in all three is gated on `max_threads > 1` -- tile
// workers, row workers and the loop filter alike. So the zero that three
// modules here passed "to let the library choose" was choosing one thread, and
// every VP8, VP9, AV1-by-reference and AV2 frame in this tree was decoded on a
// single core. The comment beside it said the opposite, which is how it
// survived a policy of optimising maximally.
//
// dav1d is the exception and keeps its zero: there `n_threads = 0` genuinely
// means "as many as the machine has", which is where the false belief about
// the other three came from.
//
// Capped at 64, which is libaom's MAX_NUM_THREADS and more than libvpx can put
// to use on any tile layout; a machine with more cores than that is not
// slowed by the cap, only not helped past it.
#pragma once

#include <algorithm>
#include <thread>

namespace mp {

[[nodiscard]] inline unsigned decoder_threads() noexcept
{
    const unsigned have = std::thread::hardware_concurrency();
    return std::clamp(have == 0u ? 1u : have, 1u, 64u);
}

} // namespace mp

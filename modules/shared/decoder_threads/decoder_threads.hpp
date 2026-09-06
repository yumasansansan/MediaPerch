// SPDX-License-Identifier: GPL-3.0-or-later
//
// How many threads a video decoder is told it may use, and how it is told.
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

#include <mediaperch/module.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace mp {

[[nodiscard]] inline unsigned decoder_threads() noexcept
{
    const unsigned have = std::thread::hardware_concurrency();
    return std::clamp(have == 0u ? 1u : have, 1u, 64u);
}

/// **The one `set` key a video decoder here answers**, and the rules that come
/// with it.
///
/// The parsing and the refusals are shared; the number is not. What a thread
/// count *means* belongs to the library -- dav1d's zero is one per core, the
/// other three read zero as one, and each has its own ceiling -- so this holds
/// what somebody asked for and says nothing about what to do when nobody did.
/// That stays beside the call that needs it, which is the only place that knows.
///
/// **The count is fixed once a packet has gone in.** Every decoder here takes
/// its thread count when it starts its workers, which is at or before the first
/// packet, and none of them can resize the pool afterwards. A module that said
/// yes to a number it would not use would leave a host measuring the old one
/// and writing down the new one, which is the one failure a calibration must
/// not have -- so it is `MP_ERR_BUSY` instead.
class ThreadChoice {
public:
    /// What was asked for, or zero when nobody asked.
    [[nodiscard]] unsigned chosen() const noexcept { return chosen_; }

    /// A packet has gone in. Called at the top of `decode`, every time, because
    /// cheap and unconditional beats remembering to do it once.
    void fix() noexcept { fixed_ = true; }
    [[nodiscard]] bool fixed() const noexcept { return fixed_; }

    /// The whole of `MpVideoCodecVtbl::set` for the one key that exists.
    /// `trouble` is filled on a refusal that has something to explain.
    MpResult take(const char* key, const char* value, std::string& trouble) noexcept
    {
        if (key == nullptr || value == nullptr) {
            return MP_ERR_INVALID;
        }
        if (std::strcmp(key, "threads") != 0) {
            return MP_ERR_UNSUPPORTED;
        }
        if (fixed_) {
            trouble = "threads cannot change once decoding has started";
            return MP_ERR_BUSY;
        }
        char* end = nullptr;
        const unsigned long asked = std::strtoul(value, &end, 10);
        if (end == value || end == nullptr || *end != '\0' || asked == 0) {
            trouble = "threads is a whole number of at least one";
            return MP_ERR_INVALID;
        }
        // Above every ceiling any of these libraries has, so a caller naming a
        // very large number gets the largest useful one rather than a refusal:
        // it asked for "as many as you can", which is an answer.
        chosen_ = static_cast<unsigned>(std::min<unsigned long>(asked, 64ul));
        return MP_OK;
    }

private:
    unsigned chosen_ = 0;
    bool fixed_ = false;
};

} // namespace mp

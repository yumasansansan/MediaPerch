// SPDX-License-Identifier: GPL-3.0-or-later
//
// A `describe` row read whole, as a stage has been able to give one since
// ABI v5.
//
// Every head that lists a stage's settings asked with a buffer of its own
// size -- 256 bytes in one place, 512 in another -- and a row longer than it was
// cut short where the module wrote it, and taken for whole. A stage says how
// long a row is now, and this is the one place that listens: the audio stages,
// the video stages and the presenter all read their rows through it.

#ifndef MEDIAPERCH_ROW_HPP
#define MEDIAPERCH_ROW_HPP

#include <mediaperch/module.h>

#include <cstdint>
#include <string>

namespace mp {

/// One row, through `ask(char* out, std::uint32_t out_bytes, std::uint32_t*
/// out_needed)` -- a stage's `describe` bound to its stage and its index.
///
/// MP_OK with the row in `out`, MP_END past the last row, and whatever else the
/// stage answered otherwise; MP_ERR_INTERNAL for a stage that says a row does
/// not fit in the room it asked for, which would have it asked again forever.
template <typename Ask>
MpResult read_row(Ask&& ask, std::string& out)
{
    out.clear();
    std::string buffer(256, '\0');
    std::uint32_t needed = 0;
    MpResult r = ask(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &needed);
    if (r == MP_TOO_SMALL && needed > buffer.size()) {
        buffer.resize(needed);
        r = ask(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &needed);
    }
    if (r == MP_TOO_SMALL) {
        return MP_ERR_INTERNAL;
    }
    if (r == MP_OK) {
        out.assign(buffer.c_str());
    }
    return r;
}

} // namespace mp

#endif // MEDIAPERCH_ROW_HPP

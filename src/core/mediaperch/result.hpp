// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an `MpResult` is called, in words a person can act on.
//
// In the core because more than one head needs it now: the probe prints these,
// the engine puts them in its log, and a shell shows them to somebody who is
// not going to look up a number. One table, so the three cannot disagree.

#ifndef MEDIAPERCH_RESULT_HPP
#define MEDIAPERCH_RESULT_HPP

#include <mediaperch/module.h>

namespace mp {

/// Never null, and never a bare code: "device in use by another application"
/// tells somebody what to do and `MP_ERR_BUSY` does not.
[[nodiscard]] const char* result_name(MpResult r) noexcept;

} // namespace mp

#endif // MEDIAPERCH_RESULT_HPP

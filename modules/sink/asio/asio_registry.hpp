// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which ASIO drivers the registry says are installed.
//
// Split from sink_asio.cpp for one reason: these are the calls whose SDK
// annotations the analyzer objects to -- C6553 on `RegOpenKeyExW`, in
// winreg.h, which is not this tree's to fix -- and this tree's warnings are
// errors. So the registry reading is on the other side of the
// MEDIAPERCH_EXTERNAL line, in a target with no flags of its own, and
// sink_asio.cpp keeps the full set. The hole is exactly this file and no wider.
//
// Written here rather than taken from the SDK's `asiolist.cpp`, which is 1990s
// code with fixed `char[32]` buffers in it. This tree opens its own files for
// the same reason.

#pragma once

#include <string>
#include <vector>

namespace mp::asio {

/// One registered driver, as the registry describes it.
struct Registered {
    std::wstring key;   ///< the subkey name, which is what a person sees
    std::wstring clsid; ///< "{...}", and the id the sink hands out
    std::wstring description;
    /// The driver DLL, from `InprocServer32`. Read here because the sink loads
    /// it itself; `sink_open` says why.
    std::wstring dll;
};

/// Every driver under HKLM\SOFTWARE\ASIO, in the order the registry gives.
[[nodiscard]] std::vector<Registered> registered_drivers();

} // namespace mp::asio

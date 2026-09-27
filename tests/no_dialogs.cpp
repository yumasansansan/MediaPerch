// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every test program fails on stderr and in its exit code, never in a window:
// see src/win/mediaperch/no_dialogs.hpp. A file of its own because the test
// binary's `main` is Catch2's, and an object at namespace scope is constructed
// before any `main` runs.

#include "mediaperch/no_dialogs.hpp"

namespace {

struct NoDialogs {
    NoDialogs() noexcept { mp::win::no_dialogs(); }
};

const NoDialogs no_dialogs_before_main;

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <avrt.h>
#include <objbase.h>
// CommandLineToArgvW: the only correct splitter for Windows quoting rules.
#include <shellapi.h>

#include <bit>
#include <type_traits>

namespace mp::win {

/// The function `name` that `module` exports, as the type the caller knows it
/// to have, or null.
///
/// **The one place the Windows head turns a function pointer into another
/// type.** GetProcAddress answers every name as a FARPROC, and a function can
/// only be called through a pointer of its own type, so what it answers has to
/// be converted -- and no form of the conversion can check it: that the export
/// has the type asked for is the DLL's documentation's promise. So it is made
/// here once, for pointers to functions only.
template <typename Function>
[[nodiscard]] Function exported(HMODULE module, const char* name) noexcept
{
    static_assert(std::is_pointer_v<Function> &&
                      std::is_function_v<std::remove_pointer_t<Function>>,
                  "exported() is for a pointer to a function");
    return std::bit_cast<Function>(::GetProcAddress(module, name));
}

} // namespace mp::win

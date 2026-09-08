// SPDX-License-Identifier: GPL-3.0-or-later
//
// A UTF-8 path, as the Windows file functions want it: wide, and past MAX_PATH
// with the prefix that lifts the limit.
//
// **Why it exists.** Every module here opened files through its own copy of
// the same fifteen lines -- UTF-8 to UTF-16, then `_wfopen_s` -- and every
// copy stopped at 260 characters, because that is where the file functions
// stop unless a path arrives as `\\?\C:\...`: backslashes only, no `..`, and
// no limit but the file system's 32767. The HDR10 test patterns' folder tree
// is fifty characters past the limit at its deepest, and a whole folder of
// calibration patterns was "nothing here reads it" for that reason alone,
// with nothing in the log to say so. Beside module_log.hpp because that is
// the one header every module already includes; the Windows head carries the
// same rule in platform.cpp's `open_utf8`, written there rather than included
// from here so that src/win does not include modules/shared.

#pragma once

#include <cstdio>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mp::winpath {

/// UTF-8 to UTF-16, or empty when there is nothing or the bytes will not
/// convert.
inline std::wstring widen(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == '\0') {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

/// **The path a file function will open at any length.** A path that fits
/// MAX_PATH with its terminator is handed over as it is. A longer one gets the
/// extended-length prefix, which the file functions take literally: the
/// slashes are turned round first, and a path with a `.` or `..` segment in it
/// -- which the prefix would not resolve -- or one that is not absolute is
/// left alone, to fail the way it always did rather than to open the wrong
/// file. A share becomes `\\?\UNC\server\share\...`.
inline std::wstring for_open(const char* utf8)
{
    std::wstring wide = widen(utf8);
    if (wide.size() < MAX_PATH || wide.rfind(L"\\\\?\\", 0) == 0) {
        return wide;
    }
    for (wchar_t& c : wide) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (wide.find(L"\\.\\") != std::wstring::npos || wide.find(L"\\..\\") != std::wstring::npos ||
        wide.size() >= 2 && wide.compare(wide.size() - 2, 2, L"\\.") == 0) {
        return wide;
    }
    if (wide.size() >= 3 && wide[1] == L':' && wide[2] == L'\\') {
        return L"\\\\?\\" + wide;
    }
    if (wide.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + wide.substr(2);
    }
    return wide;
}

/// `_wfopen_s` over `for_open`: null when the path is not UTF-8 or will not
/// open, which is what every module's own copy answered.
inline std::FILE* fopen_utf8(const char* utf8, const wchar_t* mode)
{
    const std::wstring wide = for_open(utf8);
    if (wide.empty()) {
        return nullptr;
    }
    std::FILE* file = nullptr;
    return ::_wfopen_s(&file, wide.c_str(), mode) == 0 ? file : nullptr;
}

} // namespace mp::winpath

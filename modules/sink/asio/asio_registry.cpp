// SPDX-License-Identifier: GPL-3.0-or-later
//
// See asio_registry.hpp for why this is a file of its own.

#include "asio_registry.hpp"

#include <windows.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mp::asio {

namespace {

/// The DLL a CLSID's `InprocServer32` names, or empty.
std::wstring inproc_server(const std::wstring& clsid)
{
    if (clsid.empty()) {
        return {};
    }
    const std::wstring path = L"CLSID\\" + clsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t data[1024];
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LSTATUS r = ::RegQueryValueExW(key, nullptr, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(data), &bytes);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    const std::size_t chars = bytes / sizeof(wchar_t);
    data[chars < 1024 ? chars : 1023] = L'\0';
    return std::wstring{data};
}

/// Every driver under HKLM\\SOFTWARE\\ASIO, in the order the registry gives.
///
/// Written here rather than taken from the SDK's `asiolist.cpp`, which is 1990s
/// code with fixed `char[32]` buffers in it. This tree opens its own files for
/// the same reason.
} // namespace

std::vector<Registered> registered_drivers()
{
    std::vector<Registered> out;
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return out;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD name_len = 256;
        if (::RegEnumKeyExW(root, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, name, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }
        const auto read = [&](const wchar_t* value) -> std::wstring {
            wchar_t data[512];
            DWORD bytes = sizeof(data);
            DWORD type = 0;
            if (::RegQueryValueExW(key, value, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(data), &bytes) != ERROR_SUCCESS ||
                type != REG_SZ) {
                return {};
            }
            data[(bytes / sizeof(wchar_t)) < 512 ? (bytes / sizeof(wchar_t)) : 511] = L'\0';
            return std::wstring{data};
        };
        Registered entry;
        entry.key = name;
        entry.clsid = read(L"CLSID");
        entry.description = read(L"Description");
        ::RegCloseKey(key);
        entry.dll = inproc_server(entry.clsid);
        // A key with no CLSID names nothing that can be instantiated.
        if (!entry.clsid.empty()) {
            out.push_back(std::move(entry));
        }
    }
    ::RegCloseKey(root);
    return out;
}

} // namespace mp::asio

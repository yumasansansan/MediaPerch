// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_platform.hpp on Windows. Compiled on Windows alone (tests/CMakeLists.txt
// and fuzz/CMakeLists.txt); see the header for why this is a file of its own.

#include "test_platform.hpp"

#include <bit>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace mp::test::platform {

void* open_library(const std::filesystem::path& path) noexcept
{
    return ::LoadLibraryW(path.c_str());
}

Function find_function(void* library, const char* name) noexcept
{
    return std::bit_cast<Function>(::GetProcAddress(static_cast<HMODULE>(library), name));
}

void close_library(void* library) noexcept
{
    ::FreeLibrary(static_cast<HMODULE>(library));
}

bool write_sparse(const std::filesystem::path& path, const std::vector<std::uint8_t>& head,
                  std::uint64_t tail_at, const std::vector<std::uint8_t>& tail)
{
    // NTFS makes a file sparse when asked, and only then: without the request,
    // a write past the end fills everything before it with zeros, on disk.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    bool ok = ::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                                nullptr) != FALSE;
    DWORD written = 0;
    ok = ok && ::WriteFile(file, head.data(), static_cast<DWORD>(head.size()), &written,
                           nullptr) != FALSE;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(tail_at);
    ok = ok && ::SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE;
    ok = ok && ::WriteFile(file, tail.data(), static_cast<DWORD>(tail.size()), &written,
                           nullptr) != FALSE;
    ::CloseHandle(file);
    return ok;
}

std::filesystem::path long_form(const std::filesystem::path& path)
{
    // Backslashes only: the prefix hands the rest to the file system as it is.
    return std::filesystem::path{L"\\\\?\\" + std::filesystem::path{path}.make_preferred().wstring()};
}

bool overwrite(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return true;
    }
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE mapping = ::CreateFileMappingW(handle, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    void* const view =
        mapping != nullptr ? ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : nullptr;
    if (view != nullptr) {
        std::memcpy(view, data, size);
        ::UnmapViewOfFile(view);
    }
    if (mapping != nullptr) {
        ::CloseHandle(mapping);
    }
    ::CloseHandle(handle);
    return view != nullptr;
}

unsigned long process_id() noexcept
{
    return static_cast<unsigned long>(::GetCurrentProcessId());
}

bool process_running(unsigned long id) noexcept
{
    const HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
    if (process == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(process, &code) != FALSE && code == STILL_ACTIVE;
    ::CloseHandle(process);
    return alive;
}

void* open_timer() noexcept
{
    return ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    SYNCHRONIZE | TIMER_MODIFY_STATE);
}

bool wait_timer(void* timer, std::chrono::nanoseconds left) noexcept
{
    if (timer == nullptr) {
        return false;
    }
    // Relative, which is negative, in units of 100 ns -- rounded up, so that the
    // wait does not end before the deadline.
    LARGE_INTEGER when{};
    when.QuadPart = -((left.count() + 99) / 100);
    return ::SetWaitableTimerEx(timer, &when, 0, nullptr, nullptr, nullptr, 0) != FALSE &&
           ::WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
}

void close_timer(void* timer) noexcept
{
    if (timer != nullptr) {
        ::CloseHandle(timer);
    }
}

} // namespace mp::test::platform

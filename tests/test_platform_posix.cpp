// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_platform.hpp on every system but Windows (tests/CMakeLists.txt and
// fuzz/CMakeLists.txt): POSIX for what ISO C++ has no word for -- loading a
// shared library, and asking after a process -- and ISO C++ for the rest.

#include "test_platform.hpp"

#include <bit>
#include <fstream>
#include <ios>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

namespace mp::test::platform {

void* open_library(const std::filesystem::path& path) noexcept
{
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

Function find_function(void* library, const char* name) noexcept
{
    return std::bit_cast<Function>(::dlsym(library, name));
}

void close_library(void* library) noexcept
{
    ::dlclose(library);
}

bool write_sparse(const std::filesystem::path& path, const std::vector<std::uint8_t>& head,
                  std::uint64_t tail_at, const std::vector<std::uint8_t>& tail)
{
    // Nothing to ask for: ext4, XFS, Btrfs and tmpfs, which are where a Linux
    // temporary directory lives, leave a hole where a write past the end
    // skipped, and a hole reads back as zeros and takes no disk.
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(head.data()),
               static_cast<std::streamsize>(head.size()));
    file.seekp(static_cast<std::streamoff>(tail_at));
    file.write(reinterpret_cast<const char*>(tail.data()),
               static_cast<std::streamsize>(tail.size()));
    file.close();
    return !file.fail();
}

std::filesystem::path long_form(const std::filesystem::path& path)
{
    return path; // no limit to lift
}

bool overwrite(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size)
{
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    file.close();
    return !file.fail();
}

unsigned long process_id() noexcept
{
    return static_cast<unsigned long>(::getpid());
}

bool process_running(unsigned long id) noexcept
{
    return ::kill(static_cast<pid_t>(id), 0) == 0;
}

void* open_timer() noexcept
{
    return nullptr; // std::this_thread::sleep_until keeps time here
}

bool wait_timer(void* /*timer*/, std::chrono::nanoseconds /*left*/) noexcept
{
    return false;
}

void close_timer(void* /*timer*/) noexcept {}

} // namespace mp::test::platform

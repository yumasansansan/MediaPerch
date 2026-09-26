// SPDX-License-Identifier: GPL-3.0-or-later
//
// A name in the temporary directory that no other test process picks too.
//
// **ctest runs every test case as a process of its own, and with -j several of
// them at once**, all writing into one temporary directory. Three tests named
// their files by themselves, and each way failed the same way: a counter kept
// in the process starts at 1 in every process, a fixed name is the same in all
// of them, and a fixed directory is removed by whichever process finishes
// first. Two of the LUT tests, run side by side, wrote and removed each other's
// table, and one of them read the other's picture back. The process id is what
// tells two processes apart while both run, and the counter tells one
// process's own files apart; a name left behind by a process long gone, whose
// id came round again, is simply written over.

#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace mp::test {

/// `mediaperch-<process id>-<n>-<name>`: `name`, as the test would have called
/// its file or directory, made this process's and this call's alone.
[[nodiscard]] inline std::string unique_name(std::string_view name)
{
    static std::atomic<unsigned> counter{0};
#ifdef _WIN32
    const auto id = static_cast<unsigned long>(_getpid());
#else
    const auto id = static_cast<unsigned long>(getpid());
#endif
    return "mediaperch-" + std::to_string(id) + "-" + std::to_string(++counter) + "-" +
           std::string{name};
}

/// `unique_name(name)` in the temporary directory.
[[nodiscard]] inline std::filesystem::path temp_path(std::string_view name)
{
    return std::filesystem::temp_directory_path() / unique_name(name);
}

} // namespace mp::test

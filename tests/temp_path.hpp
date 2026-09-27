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
// table, and one of them read the other's picture back. A number drawn once per
// process from std::random_device is what tells two processes apart while both
// run -- sixty-four bits, which two processes of one test run do not both draw
// -- and the counter tells one process's own files apart.
//
// It was the process id, which asked the system, and so asked it by name on
// each system. ISO C++ says this much by itself.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace mp::test {

/// `mediaperch-<process token>-<n>-<name>`: `name`, as the test would have
/// called its file or directory, made this process's and this call's alone.
[[nodiscard]] inline std::string unique_name(std::string_view name)
{
    static const std::uint64_t token = [] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }();
    static std::atomic<unsigned> counter{0};
    return "mediaperch-" + std::to_string(token) + "-" + std::to_string(++counter) + "-" +
           std::string{name};
}

/// `unique_name(name)` in the temporary directory.
[[nodiscard]] inline std::filesystem::path temp_path(std::string_view name)
{
    return std::filesystem::temp_directory_path() / unique_name(name);
}

} // namespace mp::test

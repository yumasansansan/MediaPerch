// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the tests ask of the system, declared without it.
//
// **The system is chosen by the build, not by the preprocessor**, as it is for
// the modules (modules/shared/module_log/module_file.hpp): this header is ISO
// C++, test_platform_windows.cpp is Windows' answer to each question and
// test_platform_posix.cpp every other system's, and tests/CMakeLists.txt
// compiles the one that fits. No test and no helper a test includes has an
// `#if` for the system in it. What ISO C++ can say on every system is not
// here at all, but written in ISO C++ where it is used.

#pragma once

#include <mediaperch/module.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mp::test {

/// `count` bytes into `out`, a page at a time. A std::ostream writes chars and a
/// byte is not one, so the bytes are copied into characters rather than looked
/// at as them; a page of copying costs nothing beside the write it goes to.
inline void write_bytes(std::ostream& out, const std::uint8_t* bytes, std::size_t count)
{
    std::array<char, 4096> page{};
    while (count > 0) {
        const std::size_t now = std::min(count, page.size());
        std::transform(bytes, bytes + now, page.begin(),
                       [](std::uint8_t byte) { return static_cast<char>(byte); });
        out.write(page.data(), static_cast<std::streamsize>(now));
        bytes += now;
        count -= now;
    }
}

/// A path CMake or a module wrote, which is UTF-8. A `char` string on its own
/// is the ANSI code page to std::filesystem on Windows; this is ISO C++'s way
/// of saying UTF-8 on every system.
[[nodiscard]] inline std::filesystem::path utf8_path(std::string_view utf8)
{
    return std::filesystem::path{std::u8string(utf8.begin(), utf8.end())};
}

/// `path` as UTF-8 bytes, which is what the module ABI takes.
[[nodiscard]] inline std::string utf8_string(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace mp::test

namespace mp::test::platform {

// ---------------------------------------------------------------------------
// Shared libraries. A module is one -- a DLL on Windows, a shared object
// elsewhere -- and ISO C++ has no word for loading one.

/// The library at `path`, loaded, or null.
[[nodiscard]] void* open_library(const std::filesystem::path& path) noexcept;
/// The module entry the library exports (MP_MODULE_ENTRY_NAME), as the type
/// the ABI gives it, or null. The system hands back every exported function as
/// one type, and a function is called through a pointer of its own; the one
/// conversion between the two is made here, with the name that says which
/// function it is, rather than by each caller.
[[nodiscard]] MpModuleEntry find_module_entry(void* library) noexcept;
/// Lets the library go.
void close_library(void* library) noexcept;

// ---------------------------------------------------------------------------
// Files.

/// A file of `head` at its start and `tail` at `tail_at`, with nothing on disk
/// between them: sparse. False when the file system will not make it so, which
/// is a volume a test is not going to fill with gibibytes of zeros.
[[nodiscard]] bool write_sparse(const std::filesystem::path& path,
                                const std::vector<std::uint8_t>& head, std::uint64_t tail_at,
                                const std::vector<std::uint8_t>& tail);

/// `path` as the file functions take one past MAX_PATH: with the `\\?\` prefix
/// on Windows, whose functions stop at MAX_PATH without it, and as it is on
/// every other system.
[[nodiscard]] std::filesystem::path long_form(const std::filesystem::path& path);

/// `size` bytes written over a file that exists and is exactly that long.
/// On Windows through a mapping, which the virus scanner does not read when the
/// file is next opened (tests/scratch_files.hpp has the measurement); written
/// as any file is everywhere else.
[[nodiscard]] bool overwrite(const std::filesystem::path& path, const std::uint8_t* data,
                             std::size_t size);

// ---------------------------------------------------------------------------
// Processes.

/// This process's id.
[[nodiscard]] unsigned long process_id() noexcept;
/// Whether a process of that id is running.
[[nodiscard]] bool process_running(unsigned long id) noexcept;

// ---------------------------------------------------------------------------
// Time.

/// A timer of the system's that wakes a thread nearer its time than
/// `std::this_thread::sleep_until` does, or null where there is none worth
/// having: Windows' high-resolution waitable timer, and nothing elsewhere.
[[nodiscard]] void* open_timer() noexcept;
/// Waits `left` on such a timer. False when it could not, and the caller
/// sleeps the standard way instead.
[[nodiscard]] bool wait_timer(void* timer, std::chrono::nanoseconds left) noexcept;
/// Lets it go; null is nothing to let go.
void close_timer(void* timer) noexcept;

/// Sleeps a thread to a deadline, as near to it as the system can wake one.
///
/// **On Windows, by a timer that can keep one.** `std::this_thread::sleep_until`
/// sleeps there in whole timer ticks, 15.5 ms, so a device paced by it played
/// 31 periods back to back once a tick: the right rate on average, and a burst
/// that no ring sized for this device lives through -- calibration's rings of
/// 4 and 8 ms underran in every run. A waitable timer made high-resolution
/// wakes within tens of microseconds of its time: paced by one, 256 periods of
/// 500 us came a median of 500 us apart and never more than two back to back,
/// which is how a device's own clock runs. The flag is Windows 10 1803's; a
/// system older than that refuses the timer, and the ticks are what is left.
/// Elsewhere `std::this_thread::sleep_until` is the timer.
class PeriodTimer {
public:
    PeriodTimer() noexcept : timer_(open_timer()) {}
    ~PeriodTimer() { close_timer(timer_); }
    PeriodTimer(const PeriodTimer&) = delete;
    PeriodTimer& operator=(const PeriodTimer&) = delete;

    void sleep_until(std::chrono::steady_clock::time_point due) const noexcept
    {
        const auto left = due - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            return;
        }
        if (!wait_timer(timer_, std::chrono::duration_cast<std::chrono::nanoseconds>(left))) {
            std::this_thread::sleep_until(due);
        }
    }

private:
    void* timer_;
};

} // namespace mp::test::platform

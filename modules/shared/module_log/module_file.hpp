// SPDX-License-Identifier: GPL-3.0-or-later
//
// A file a module opens by name: by the UTF-8 path the ABI carries, at any
// length, and with a position of 64 bits -- on every system the modules build
// for, and with no system in this header.
//
// **Two things ISO C cannot say on Windows, and both are here.** A path in
// UTF-8 has to become UTF-16 before Windows' file functions will take it, and
// past MAX_PATH it needs the prefix that lifts the limit (win_path.hpp); and
// `std::fseek` and `std::ftell` take and give a `long`, which 64-bit Windows
// keeps at 32 bits -- its data model is LLP64, where only `long long` and
// pointers are 64 bits -- so a file past two gibibytes has positions they
// cannot say. Every module that opens a file had its own `#if defined(_WIN32)`
// for the first, and six called the MSVC runtime's `_fseeki64` by name for the
// second. Elsewhere ISO C says all of it: a path is bytes, and 64-bit Linux is
// LP64, with a `long` of 64 bits.
//
// **So the system is chosen by the build, not by the preprocessor.** This
// header declares, module_file_windows.cpp is the Windows functions and
// module_file_iso.cpp is ISO C's and nothing else, and CMakeLists.txt compiles
// the one that fits: a module is the same source on every system, and the
// code that is not is in a file of its own. Beside module_log.hpp, as
// win_path.hpp is, because that is the library every module already links.

#pragma once

#include <cstdint>
#include <cstdio>

namespace mp::file {

/// The file at `utf8_path`, opened to read bytes (`"rb"`), or null when the
/// path is null, empty, not UTF-8 or does not open.
[[nodiscard]] std::FILE* open_read(const char* utf8_path) noexcept;

/// Moves `file` to `offset` from `whence` (SEEK_SET, SEEK_CUR or SEEK_END).
/// Zero on success, as `std::fseek`.
int seek(std::FILE* file, std::int64_t offset, int whence) noexcept;

/// Where `file` is, or -1 on failure, as `std::ftell`.
[[nodiscard]] std::int64_t tell(std::FILE* file) noexcept;

} // namespace mp::file

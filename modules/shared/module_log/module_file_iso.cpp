// SPDX-License-Identifier: GPL-3.0-or-later
//
// module_file.hpp in ISO C and nothing else, for every system but Windows
// (CMakeLists.txt): a path is bytes, handed to `std::fopen` as it came, and a
// position is a `long`, which holds every position of a file where `long` is
// 64 bits. The assertion is that condition, so a system where it does not hold
// is told so by the compiler rather than by a file past two gibibytes.

#include "module_file.hpp"

static_assert(sizeof(long) >= sizeof(std::int64_t),
              "std::fseek and std::ftell need a long of 64 bits: module_file_iso.cpp is for "
              "a system whose data model gives it one");

namespace mp::file {

std::FILE* open_read(const char* utf8_path) noexcept
{
    if (utf8_path == nullptr || *utf8_path == '\0') {
        return nullptr;
    }
    return std::fopen(utf8_path, "rb");
}

int seek(std::FILE* file, std::int64_t offset, int whence) noexcept
{
    return std::fseek(file, static_cast<long>(offset), whence);
}

std::int64_t tell(std::FILE* file) noexcept
{
    return static_cast<std::int64_t>(std::ftell(file));
}

} // namespace mp::file

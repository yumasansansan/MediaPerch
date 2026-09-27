// SPDX-License-Identifier: GPL-3.0-or-later
//
// module_file.hpp on Windows: the path through win_path.hpp -- UTF-16, and past
// MAX_PATH the prefix that lifts the limit -- and the position through the MSVC
// runtime's 64-bit pair. Compiled on Windows alone (CMakeLists.txt); see the
// header for why this is a file of its own.

#include "module_file.hpp"

#include "win_path.hpp"

namespace mp::file {

std::FILE* open_read(const char* utf8_path) noexcept
{
    // The conversion allocates, and a module's open is called across a C ABI,
    // which an exception must not cross: out of memory is a file that did not
    // open, as it is to `_wfopen_s`.
    try {
        return mp::winpath::fopen_utf8(utf8_path, L"rb");
    } catch (...) {
        return nullptr;
    }
}

int seek(std::FILE* file, std::int64_t offset, int whence) noexcept
{
    return ::_fseeki64(file, offset, whence);
}

std::int64_t tell(std::FILE* file) noexcept
{
    return ::_ftelli64(file);
}

} // namespace mp::file

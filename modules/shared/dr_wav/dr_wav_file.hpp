// SPDX-License-Identifier: GPL-3.0-or-later
//
// dr_wav reading a `FILE*` this tree opened, through module_file.hpp: the ABI's
// UTF-8 path at any length, and positions of 64 bits, on every system.
//
// dr_wav's own file functions were the other choice, and a reader had to pick
// between two of them by system: `drwav_init_file` opens a narrow path, which
// on Windows is the ANSI code page, and `drwav_init_file_w` a wide one, which
// exists on Windows alone -- so both readers held an `#if defined(_WIN32)`
// around the call. Over these callbacks the call is the same everywhere, and
// what differs by system is where module_file.hpp says it is.

#pragma once

#include "module_file.hpp"

#include <dr_wav.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mp::drwav_file {

/// dr_wav's read, over the `FILE*` it was handed.
inline std::size_t read(void* file, void* out, std::size_t bytes)
{
    return std::fread(out, 1, bytes, static_cast<std::FILE*>(file));
}

/// dr_wav's seek, in its words for the three origins.
inline drwav_bool32 seek(void* file, int offset, drwav_seek_origin origin)
{
    int whence = SEEK_SET;
    if (origin == DRWAV_SEEK_CUR) {
        whence = SEEK_CUR;
    } else if (origin == DRWAV_SEEK_END) {
        whence = SEEK_END;
    }
    return mp::file::seek(static_cast<std::FILE*>(file), offset, whence) == 0 ? DRWAV_TRUE
                                                                             : DRWAV_FALSE;
}

/// dr_wav's tell, which past two gibibytes a `long` could not have said.
inline drwav_bool32 tell(void* file, drwav_int64* cursor)
{
    const std::int64_t at = mp::file::tell(static_cast<std::FILE*>(file));
    if (at < 0) {
        return DRWAV_FALSE;
    }
    *cursor = at;
    return DRWAV_TRUE;
}

/// `wav` over `file`, which stays the caller's: dr_wav closes a file it was
/// handed callbacks for neither when this fails nor in `drwav_uninit`, so the
/// caller closes it after either. False for a null `file`, as for one dr_wav
/// cannot read.
inline bool init(drwav* wav, std::FILE* file)
{
    return file != nullptr && drwav_init(wav, read, seek, tell, file, nullptr) != DRWAV_FALSE;
}

} // namespace mp::drwav_file

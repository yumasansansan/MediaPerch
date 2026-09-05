// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module's line to the host's log, formatted once.
//
// **Twelve modules had their own copy and the buffers had drifted to three
// sizes**: 256 in `codec_mpa`, 512 in most, 1024 in `demux_ffmpeg`. The same
// message therefore said different things depending on which module wrote it,
// and a module that grew a longer diagnostic than its own buffer lost the end
// of it without saying so. That is the same failure `sample_type_for` had, and
// it is why modules/shared exists.
//
// **The varargs wrapper stays in each module and only its body moves here**,
// because a `...` function cannot forward to another `...` function -- the
// va_list has to be made where the arguments are. What is shared is the part
// that was wrong: one buffer size, one null check, one call.
//
// 1024 bytes, which is the largest of the three that were in use, so nothing
// that fits today starts being cut.

#ifndef MEDIAPERCH_SHARED_MODULE_LOG_HPP
#define MEDIAPERCH_SHARED_MODULE_LOG_HPP

#include <mediaperch/module.h>

#include <cstdarg>

namespace mp::log {

/// One line, verbatim. Does nothing when the host has no log, which is what a
/// module loaded by a test harness usually has.
void line(const MpHost* host, MpLogLevel level, const char* msg) noexcept;

/// One formatted line. Called from a module's own `log_fmt`, which is where the
/// `va_list` can be made.
void vfmt(const MpHost* host, MpLogLevel level, const char* format,
          std::va_list args) noexcept;

} // namespace mp::log

#endif // MEDIAPERCH_SHARED_MODULE_LOG_HPP

// SPDX-License-Identifier: GPL-3.0-or-later
#include "module_log.hpp"

#include <cstdio>

namespace mp::log {

void line(const MpHost* host, MpLogLevel level, const char* msg) noexcept
{
    if (host == nullptr || host->log == nullptr || msg == nullptr) {
        return;
    }
    host->log(host->ctx, level, msg);
}

void vfmt(const MpHost* host, MpLogLevel level, const char* format,
          std::va_list args) noexcept
{
    if (host == nullptr || host->log == nullptr || format == nullptr) {
        return;
    }
    // **Truncation is silent and that is the right trade here.** A diagnostic
    // that does not fit is still worth most of, and a module cannot allocate on
    // every path that logs -- MP_RT forbids it outright.
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    host->log(host->ctx, level, buffer);
}

} // namespace mp::log

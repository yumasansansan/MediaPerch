// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/result.hpp"

namespace mp {

const char* result_name(MpResult r) noexcept
{
    switch (r) {
    case MP_OK: return "ok";
    case MP_END: return "end";
    case MP_ERR_INVALID: return "invalid";
    case MP_ERR_UNSUPPORTED: return "unsupported by this module";
    case MP_ERR_FORMAT: return "format refused";
    case MP_ERR_IO: return "io";
    case MP_ERR_DEVICE_LOST: return "device lost";
    case MP_ERR_BUSY: return "device in use by another application";
    case MP_ERR_DENIED: return "exclusive mode is disabled for this device";
    case MP_ERR_NO_MEMORY: return "out of memory";
    case MP_TIMEOUT: return "timed out";
    default: return "internal error";
    }
}

} // namespace mp

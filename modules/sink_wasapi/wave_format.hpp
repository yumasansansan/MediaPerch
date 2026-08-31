// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "win_headers.hpp"

#include <mediaperch/module.h>

namespace mp::wasapi {

/// Build the `WAVEFORMATEX(TENSIBLE)` a device is asked for.
///
/// Two shapes, and which one is used is not a style choice:
///
///   - the plain `WAVEFORMATEX` when every bit of the container carries signal
///     and the layout is the default. Some older drivers accept only this;
///   - `WAVEFORMATEXTENSIBLE` whenever `wValidBitsPerSample` differs from
///     `wBitsPerSample`, or a channel mask was asked for, or there are more than
///     two channels. 24-in-32 *cannot* be expressed any other way -- plain PCM
///     at 32 bits means 32 valid bits, which is a different format.
///
/// `dwChannelMask` may legitimately be zero, meaning "unspecified". Drivers that
/// insist on a real layout reject that, which is exactly what the next candidate
/// in the list is for.
///
/// Returns false for anything that cannot be expressed -- today that is
/// IEC 61937 bitstreams, which need a subformat per codec and have no source to
/// feed them yet.
bool to_wave_format(const MpFormat& format, WAVEFORMATEXTENSIBLE& out) noexcept;

/// Bytes one frame occupies, or 0 for a format with no wire representation.
[[nodiscard]] UINT32 frame_bytes_of(const MpFormat& format) noexcept;

/// Read a device-supplied format back. Used for shared mode, where the engine
/// rather than the driver decides, and for reporting what a device is set to.
bool from_wave_format(const WAVEFORMATEX& wfx, MpFormat& out) noexcept;

} // namespace mp::wasapi

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one place Windows headers are included in this module, in the one order
// that works.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <functiondiscoverykeys_devpkey.h>

#include <wrl/client.h>

namespace mp::wasapi {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// The KSDATAFORMAT_SUBTYPE_* values, written out rather than pulled in from
// <ksmedia.h>. That header drags in <ks.h>, which is particular about include
// order and about what has already defined; these two GUIDs are stable, are
// documented, and are all of it we need.
inline constexpr GUID subtype_pcm{
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID subtype_ieee_float{
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

} // namespace mp::wasapi

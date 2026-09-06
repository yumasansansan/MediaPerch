// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/refresh_win.hpp"

#include <dxgi1_2.h>

#include <cstdio>
#include <utility>

#pragma comment(lib, "dxgi.lib")

namespace mp::win {
namespace {

/// The DXGI output whose monitor contains `window`, or the first one.
///
/// The same rule `VBlankClock::open` follows and for the same reason: on two
/// monitors at different rates, the neighbour's mode list is the wrong answer.
IDXGIOutput* output_for(void* window, DXGI_OUTPUT_DESC& desc)
{
    HMONITOR monitor =
        window != nullptr
            ? ::MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST)
            : nullptr;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    IDXGIOutput* found = nullptr;
    IDXGIOutput* first = nullptr;
    DXGI_OUTPUT_DESC first_desc{};
    for (UINT a = 0; found == nullptr; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT o = 0;; ++o) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC one{};
            output->GetDesc(&one);
            if (monitor != nullptr && one.Monitor == monitor) {
                found = output;
                desc = one;
                break;
            }
            if (first == nullptr) {
                first = output;
                first_desc = one;
            } else {
                output->Release();
            }
        }
        adapter->Release();
    }
    factory->Release();
    if (found == nullptr) {
        found = first;
        desc = first_desc;
    } else if (first != nullptr) {
        first->Release();
    }
    return found;
}

struct Config {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool query_active(Config& out)
{
    UINT32 np = 0;
    UINT32 nm = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
        return false;
    }
    out.paths.resize(np);
    out.modes.resize(nm);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, out.paths.data(), &nm,
                             out.modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }
    out.paths.resize(np);
    out.modes.resize(nm);
    return !out.paths.empty();
}

/// Which active path drives the display `window` is on.
///
/// Matched by the adapter's own device name, because that is the one string
/// both APIs speak: `DXGI_OUTPUT_DESC::DeviceName` and the CCD API's
/// `DISPLAYCONFIG_SOURCE_DEVICE_NAME::viewGdiDeviceName` are both `\\.\DISPLAYn`.
std::size_t path_for(const Config& config, const wchar_t* device)
{
    for (std::size_t i = 0; i < config.paths.size(); ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = config.paths[i].sourceInfo.adapterId;
        name.header.id = config.paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
            continue;
        }
        if (::lstrcmpW(name.viewGdiDeviceName, device) == 0) {
            return i;
        }
    }
    return 0; // the primary, which is the honest fallback for a single display
}

const char* said(LONG r)
{
    switch (r) {
    case ERROR_SUCCESS:
        return "";
    case ERROR_INVALID_PARAMETER:
        return "the display configuration was refused as malformed";
    case ERROR_NOT_SUPPORTED:
        return "this adapter does not support setting a mode that way";
    case ERROR_ACCESS_DENIED:
        return "another process holds the display configuration";
    case ERROR_GEN_FAILURE:
        return "the driver would not take it";
    default:
        return "the display refused the mode";
    }
}

/// Asks for `refresh` on `path`, validating first.
///
/// **The mode index is cleared, not the bitfield beside it.** `modeInfoIdx` is
/// the 32-bit union member; `targetModeInfoIdx` is a 16-bit field sharing those
/// bytes with `desktopModeInfoIdx`. Writing the narrow one leaves the desktop
/// index pointing at a mode that no longer matches, and every request comes
/// back ERROR_INVALID_PARAMETER with nothing to say which field was wrong. That
/// cost an afternoon.
LONG put(Rational refresh, const wchar_t* device, bool apply)
{
    Config config;
    if (!query_active(config)) {
        return ERROR_NOT_FOUND;
    }
    const std::size_t at = path_for(config, device);
    config.paths[at].targetInfo.refreshRate.Numerator = refresh.num;
    config.paths[at].targetInfo.refreshRate.Denominator = refresh.den;
    config.paths[at].targetInfo.scanLineOrdering =
        DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    config.paths[at].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    config.paths[at].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

    const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                         (apply ? SDC_APPLY : SDC_VALIDATE);
    return ::SetDisplayConfig(static_cast<UINT32>(config.paths.size()),
                              config.paths.data(), 0, nullptr, flags);
}

Rational active_refresh(const wchar_t* device)
{
    Config config;
    if (!query_active(config)) {
        return {};
    }
    const std::size_t at = path_for(config, device);
    const auto& r = config.paths[at].targetInfo.refreshRate;
    return Rational{r.Numerator, r.Denominator};
}

} // namespace

std::vector<DisplayMode> display_modes(void* window, std::string& why)
{
    std::vector<DisplayMode> out;
    DXGI_OUTPUT_DESC desc{};
    IDXGIOutput* output = output_for(window, desc);
    if (output == nullptr) {
        why = "no display output to ask";
        return out;
    }
    UINT count = 0;
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, nullptr)) ||
        count == 0) {
        why = "the output would not list its modes";
        output->Release();
        return out;
    }
    std::vector<DXGI_MODE_DESC> modes(count);
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count,
                                          modes.data()))) {
        why = "the output would not list its modes";
        output->Release();
        return out;
    }
    output->Release();
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        out.push_back(DisplayMode{modes[i].Width, modes[i].Height,
                                  Rational{modes[i].RefreshRate.Numerator,
                                           modes[i].RefreshRate.Denominator}});
    }
    return out;
}

std::optional<DisplayMode> current_mode(void* window, std::string& why)
{
    DXGI_OUTPUT_DESC desc{};
    IDXGIOutput* output = output_for(window, desc);
    if (output == nullptr) {
        why = "no display output to ask";
        return std::nullopt;
    }
    output->Release();
    const Rational refresh = active_refresh(desc.DeviceName);
    if (!refresh.valid()) {
        why = "the display configuration would not say what is in force";
        return std::nullopt;
    }
    return DisplayMode{
        static_cast<std::uint32_t>(desc.DesktopCoordinates.right -
                                   desc.DesktopCoordinates.left),
        static_cast<std::uint32_t>(desc.DesktopCoordinates.bottom -
                                   desc.DesktopCoordinates.top),
        refresh};
}

RefreshSwitch::~RefreshSwitch()
{
    restore();
}

bool RefreshSwitch::apply(void* window, Rational refresh, std::string& why)
{
    if (!refresh.valid()) {
        why = "that is not a refresh rate";
        return false;
    }
    DXGI_OUTPUT_DESC desc{};
    IDXGIOutput* output = output_for(window, desc);
    if (output == nullptr) {
        why = "no display output to switch";
        return false;
    }
    output->Release();

    const Rational was = active_refresh(desc.DeviceName);
    if (!was.valid()) {
        why = "the display configuration would not say what is in force";
        return false;
    }
    // Asked before it is done, so a mode that would not have worked is a
    // sentence rather than a black screen.
    const LONG checked = put(refresh, desc.DeviceName, false);
    if (checked != ERROR_SUCCESS) {
        why = said(checked);
        return false;
    }
    const LONG applied = put(refresh, desc.DeviceName, true);
    if (applied != ERROR_SUCCESS) {
        why = said(applied);
        return false;
    }
    if (!held_) {
        // Only the first switch records where to go back to; a second one in
        // the same run must not make the intermediate mode the destination.
        was_ = was;
        window_ = window;
        held_ = true;
    }
    // Read back rather than assumed: what is in force is a fact.
    in_force_ = active_refresh(desc.DeviceName);
    return true;
}

void RefreshSwitch::restore() noexcept
{
    if (!held_) {
        return;
    }
    held_ = false;
    DXGI_OUTPUT_DESC desc{};
    IDXGIOutput* output = output_for(window_, desc);
    if (output == nullptr) {
        return;
    }
    output->Release();
    (void)put(was_, desc.DeviceName, true);
    in_force_ = {};
}

} // namespace mp::win

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presentation: Direct3D 11, the flip model, and scRGB.
//
// **The first MpVideoVtbl**, and the half of §9 that needs a device. The other
// half -- which swap chain format, which tone mapper, how much to scale SDR
// content by -- is colour_plan.hpp, which is arithmetic and is tested without
// one. This file does what that decides.
//
// Three things shape it.
//
// **It renders off-screen when there is no window, and that is not a degraded
// mode.** A renderer judged by whether it looks plausible is how the OS tone
// mapper's 2.4 gamma survived years of bug reports (§9.2); this one renders to
// a texture a test can read back and hash, on the same path as the one that
// reaches a display. `read_back` is in the ABI for exactly that, and doubles as
// the screenshot people want anyway -- and it hands back the pixels in the
// format they were rendered in, because a measurement that quantises before it
// is taken is measuring the quantiser.
//
// **The arithmetic is single precision and the destination is as wide as it is
// allowed to be.** HLSL `float` is 32-bit and this file uses no `half` and no
// `min16float`, so every transfer function and every scale is computed at full
// single precision and rounded exactly once, when it is written. Where that
// write goes is the only thing that varies: a flip-model swap chain accepts
// nothing above `R16G16B16A16_FLOAT`, which is DXGI's ceiling rather than a
// decision, while an off-screen target takes `R32G32B32A32_FLOAT` and is what a
// measurement should land in.
//
// **That ceiling is a real loss and not a rounding to be waved through.** Half
// is relatively precise -- a step of 1/1024 of the value at worst -- and a
// 12-bit output needs 1/1706 at white, so it is already short there and 27
// times short at 16 bits. There is no way past it on the desktop: the DWM
// composites in FP16 scRGB itself. The way past it is not to use the desktop,
// which is what `sink_asio` is to `sink_wasapi` and what a presenter on a
// DeckLink or a Kona would be to this one -- rendering FP32 straight into the
// card's integer format with no half anywhere. The `device` setting below is
// deliberately a string rather than a flag so that such a module needs no new
// entry point. plan.md §9.10 has the table.
//
// There is **no intermediate render target yet, and the reason is not the one
// that first suggested itself.** An intermediate does not round twice: the
// shader writes single precision into it exactly, and the copy to the back
// buffer rounds once, which is the same one rounding as writing straight
// there. What it costs is a full-screen copy, and what it buys is a windowed
// session that can still read back single precision -- monitor at what a
// display takes, export at what the arithmetic produced. That is exactly what
// a grading tool wants, and it is not built because nothing here opens a
// window yet; off-screen already renders FP32 directly, which is the same
// thing without the copy.
//
// Double precision belongs on the CPU, where the constants are derived: a
// matrix or an EETF parameter worked out in `double` and rounded once into the
// constant buffer is exact to more digits than any display has. In a shader it
// would buy nothing measurable -- no texture format carries it, and single
// precision already has seven decimal digits against a 16-bit panel's five.
//
// **WARP is a first-class device, not a fallback for broken machines.** It is
// Microsoft's software rasteriser, it is on every Windows install, and it is
// deterministic -- so a hash taken on a CI runner with no GPU is a hash worth
// comparing. `device=warp` asks for it; the default asks the hardware first.
//
// **The swap chain is flip-model always** (§9.5), because that is what makes it
// eligible for Advanced Color processing at all. The older blit models are not
// a compatibility option here; they are a different pipeline that cannot do the
// thing this module exists to do.
//
// **The chroma arrangements are derived, not enumerated.** 4:0:0, 4:2:0,
// 4:2:2 and 4:4:4 all reach the same conversion, planar or semi-planar, at 8
// through 16 bits -- because ABI v4 made a frame describe itself and the plane
// sizes fall out of `mp_pixel_chroma_width` rather than out of a branch per
// format. Interleaved Y'CbCr (YUY2, AYUV and their neighbours) is refused: it
// is a different unpack and nothing here produces it.
//
// What is not here yet, and why. The colour work §9 turns on: the transfer
// functions here are sRGB and BT.1886, so a PQ or an HLG source is decoded
// with the wrong curve rather than refused, and the tone mappers are chosen
// and reported and not applied. §9.9.1 says HLG goes on the linear path in
// every case, which is the shape that work takes.

#include "colour_plan.hpp"
#include "kernels.hpp" // modules/shared/kernels: the chroma kernel
#include "siting.hpp"
#include "yuv_matrix.hpp"

#include <abi_guard.hpp>
#include <describe_row.hpp>
#include <mediaperch/module.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

// `QueryDisplayConfig` and the SDR white level: the CCD API, which is where
// Windows keeps what DXGI has no idea about.
#include <wingdi.h>

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d2d1_1.h>
#include <d2d1effects_2.h>
// **The SDK's own header, not held to our warning set.** `dcomp.h` declares
// `IDCompositionVisual3::SetTransform` overloads that hide the base class's
// rather than override them, which is C4263 and C4264 and is a fact about
// Windows rather than about this file. Suppressed exactly here and popped
// immediately, so ours still are errors.
#pragma warning(push)
#pragma warning(disable : 4263 4264)
#include <dcomp.h>
#pragma warning(pop)
#include <dxgi1_6.h>

namespace {

const MpHost* g_host = nullptr;

void log_line(MpLogLevel level, const char* msg) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, msg);
    }
}

/// A COM pointer small enough to read, because this file holds nine of them and
/// the alternative is nine release calls in the wrong order.
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    T** put()
    {
        reset();
        return &p_;
    }
    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept
    {
        if (p_ != nullptr) {
            p_->Release();
            p_ = nullptr;
        }
    }

    /// What std::out_ptr needs to stand for the `void**` a call writes an
    /// interface through: the call writes a `void*` of the out_ptr's own, and
    /// the out_ptr hands it here as a `T*` once the call's expression is done,
    /// its reference counted already. A `T**` seen as a `void**` would be the
    /// pointer written as another type than it is.
    using pointer = T*;
    void reset(T* p) noexcept
    {
        reset();
        p_ = p;
    }

private:
    T* p_ = nullptr;
};

// --------------------------------------------------------------------------
// The shader
// --------------------------------------------------------------------------
//
// **colour.hlsl, compiled with the build.** The HLSL, and what explains it,
// is in that file; what is here is the bytecode fxc made of each entry point
// (cmake/Shaders.cmake), which a device takes as it is. `#embed` is C23's, and
// Clang takes it in C++ as an extension it warns about: off for these lines,
// and only these.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr unsigned char k_vs_main[] = {
#embed <colour_vs_main.cso>
};
constexpr unsigned char k_ps_rgba[] = {
#embed <colour_ps_rgba.cso>
};
constexpr unsigned char k_ps_nv12[] = {
#embed <colour_ps_nv12.cso>
};
constexpr unsigned char k_ps_planar[] = {
#embed <colour_ps_planar.cso>
};
constexpr unsigned char k_ps_chroma_across[] = {
#embed <colour_ps_chroma_across.cso>
};
#pragma clang diagnostic pop

/// Mirrors the `cbuffer` in colour.hlsl, which HLSL packs in four-float rows.
struct Constants {
    float sdr_scale = 1.0f;
    float luma_offset = 0.0f;
    float luma_scale = 1.0f;
    float chroma_scale = 1.0f;

    float r_v = 0.0f;
    float g_u = 0.0f;
    float g_v = 0.0f;
    float b_u = 0.0f;

    float sample_scale = 1.0f;
    float transfer_gamma = 2.4f;
    std::uint32_t transfer_kind = 0;
    float has_chroma = 1.0f;

    float hlg_peak = 1000.0f;
    float tone_peak = 0.0f;
    std::uint32_t emit_pq = 0;
    std::uint32_t emit_linear = 0;

    std::uint32_t linear_in = 0;
    float hlg_gamma1 = 0.2f; // a 1000-nit display's 1.2, less one
    std::uint32_t gamut_mode = 1;
    std::uint32_t pad_a = 0;

    float gamut0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float gamut1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float gamut2[4] = {0.0f, 0.0f, 1.0f, 0.0f};

    float chroma_step[2] = {0.5f, 0.5f};
    float chroma_size[2] = {1.0f, 1.0f};

    std::int32_t chroma_first[4] = {0, 0, 0, 0};

    std::uint32_t chroma_taps = 8;
    std::uint32_t chroma_planar = 0;
    float tone_source_pq = 1.0f;
    float tone_source_inv = 1.0f;

    float tone_mt = 1.0f; // no roll-off until one is worked out
    float tone_ks = 1.0f;
    float tone_span_inv = 1.0f;
    float tone_peak_inv = 0.0f;

    float luma709[3] = {0.0f, 0.0f, 0.0f};
    float pad_c = 0.0f;
};
static_assert(sizeof(Constants) == 13 * 16, "thirteen rows, as the cbuffer packs them");

/// A 3x3 matrix, row by row, in double.
using Matrix3 = std::array<double, 9>;

constexpr Matrix3 multiply(const Matrix3& a, const Matrix3& b) noexcept
{
    Matrix3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[(i * 3) + j] = (a[i * 3] * b[j]) + (a[(i * 3) + 1] * b[3 + j]) +
                               (a[(i * 3) + 2] * b[6 + j]);
        }
    }
    return out;
}

constexpr Matrix3 inverse(const Matrix3& a) noexcept
{
    const double det = (a[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
                       (a[1] * ((a[3] * a[8]) - (a[5] * a[6]))) +
                       (a[2] * ((a[3] * a[7]) - (a[4] * a[6])));
    return Matrix3{((a[4] * a[8]) - (a[5] * a[7])) / det, ((a[2] * a[7]) - (a[1] * a[8])) / det,
                   ((a[1] * a[5]) - (a[2] * a[4])) / det, ((a[5] * a[6]) - (a[3] * a[8])) / det,
                   ((a[0] * a[8]) - (a[2] * a[6])) / det, ((a[2] * a[3]) - (a[0] * a[5])) / det,
                   ((a[3] * a[7]) - (a[4] * a[6])) / det, ((a[1] * a[6]) - (a[0] * a[7])) / det,
                   ((a[0] * a[4]) - (a[1] * a[3])) / det};
}

/// RGB to XYZ for a set of primaries and a white point, as SMPTE RP 177
/// derives it: the primaries' XYZ at unit luminance, each scaled so that
/// equal RGB is the white.
constexpr Matrix3 rgb_to_xyz(double xr, double yr, double xg, double yg, double xb, double yb,
                             double xw, double yw) noexcept
{
    const Matrix3 p{xr / yr, xg / yg, xb / yb, 1.0, 1.0, 1.0, (1.0 - xr - yr) / yr,
                    (1.0 - xg - yg) / yg, (1.0 - xb - yb) / yb};
    const Matrix3 q = inverse(p);
    const double w[3] = {xw / yw, 1.0, (1.0 - xw - yw) / yw};
    double s[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < 3; ++i) {
        s[i] = (q[i * 3] * w[0]) + (q[(i * 3) + 1] * w[1]) + (q[(i * 3) + 2] * w[2]);
    }
    Matrix3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[(i * 3) + j] = p[(i * 3) + j] * s[j];
        }
    }
    return out;
}

/// BT.2020 primaries to BT.709's, in linear light.
///
/// **Needed the moment PQ works**, because HDR content is graded on BT.2100 and
/// scRGB is BT.709: presenting one as the other is oversaturation, and it is
/// the kind that looks like a deliberate choice rather than a fault.
///
/// **Derived, in double, from the two recommendations' chromaticities and
/// D65**, and rounded once, into the constants: BT.2020's (0.708, 0.292),
/// (0.170, 0.797), (0.131, 0.046) into XYZ and out through BT.709's (0.640,
/// 0.330), (0.300, 0.600), (0.150, 0.060). The table it replaced was BT.2087's
/// four decimals, which are this matrix rounded to within 5e-5.
/// BT.709's primaries and D65 into XYZ: its Y row is the luminance the
/// buffer's colours have, which the gamut compression keeps.
constexpr Matrix3 k_bt709_to_xyz =
    rgb_to_xyz(0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290);
constexpr Matrix3 k_bt2020_to_bt709 =
    multiply(inverse(k_bt709_to_xyz),
             rgb_to_xyz(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290));

/// ST.2084's inverse EOTF, nits to a PQ code value, in double, for what the
/// shader is given worked out: its constants as the standard writes them,
/// ratios of small integers, as the shader's are.
[[nodiscard]] double nits_to_pq(double nits) noexcept
{
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
    return std::pow((c1 + (c2 * y)) / (1.0 + (c3 * y)), m2);
}

/// How much of the window is on this output, in pixels.
///
/// §9.4: **the output with the greatest intersection with the window**, not
/// `IDXGISwapChain::GetContainingOutput` -- that one returns a stale output, and
/// the obvious fix of recreating the swap chain flashes black.
std::uint64_t overlap(const RECT& window, const RECT& output) noexcept
{
    const LONG left = window.left > output.left ? window.left : output.left;
    const LONG top = window.top > output.top ? window.top : output.top;
    const LONG right = window.right < output.right ? window.right : output.right;
    const LONG bottom = window.bottom < output.bottom ? window.bottom : output.bottom;
    if (right <= left || bottom <= top) {
        return 0;
    }
    return static_cast<std::uint64_t>(right - left) *
           static_cast<std::uint64_t>(bottom - top);
}

/// The output the window is mostly on, or the first one when there is no
/// window to be on. False when there is none to be had.
///
/// An out-parameter rather than a return, because `Com` has no move: it is a
/// deliberately small wrapper and widening it to hold a loop's shape would be
/// the tail wagging the dog.
bool output_for(IDXGIFactory2* factory, HWND window, Com<IDXGIOutput6>& six)
{
    RECT want{};
    const bool have_window = window != nullptr && GetWindowRect(window, &want) != 0;

    // Chosen by index and fetched once, for the reason above.
    UINT best_adapter = 0;
    UINT best_output = 0;
    std::uint64_t best_area = 0;
    bool found = false;

    for (UINT a = 0;; ++a) {
        Com<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(a, adapter.put()))) {
            break;
        }
        for (UINT o = 0;; ++o) {
            Com<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, output.put()))) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) {
                continue;
            }
            // With no window there is nothing to intersect, so the first output
            // stands in -- which is what an off-screen render wants and is why
            // it is not an error.
            const std::uint64_t area =
                have_window ? overlap(want, desc.DesktopCoordinates) : 1;
            if (!found || area > best_area) {
                best_adapter = a;
                best_output = o;
                best_area = area;
                found = true;
            }
        }
        if (!have_window && found) {
            break;
        }
    }

    if (!found) {
        return false;
    }
    Com<IDXGIAdapter1> adapter;
    Com<IDXGIOutput> output;
    if (FAILED(factory->EnumAdapters1(best_adapter, adapter.put())) ||
        FAILED(adapter->EnumOutputs(best_output, output.put()))) {
        return false;
    }
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput6),
                                      std::out_ptr<void*>(six)))) {
        six.reset();
        return false;
    }
    return true;
}

/// The SDR reference white of the path this monitor is on, in nits.
///
/// **§9.6 is the one that will look wrong first**, and this is the number it
/// needs: on an HDR display scRGB 1.0 is 80 nits and is scene-referred, so an
/// OSD or an SDR film drawn at 1.0 sits dim and grey next to the video. The
/// boost has been plumbed to the pixel shader since the presenter was written
/// and was always 1, because nobody made this call.
///
/// The CCD API rather than DXGI, because DXGI has no idea: `IDXGIOutput6`
/// reports the colour space and the luminance range and says nothing about
/// where the user put the SDR slider. `src/win/refresh_win.cpp` walks the same
/// API for the refresh rate; this walks it again rather than sharing, because a
/// module gets a host vtable and not the head's code (§3).
/// What one walk of the CCD API can say about the monitor a window is on.
struct AdvancedColour {
    double sdr_white_nits = 80.0;
    /// Advanced Color is on and the mode is SDR: a wide-gamut display doing its
    /// own colour management. **§9.4 says `IDXGIOutput6` cannot tell this from a
    /// plain SDR display**, and it cannot -- but the CCD API can, and the walk
    /// for the white level is already standing in front of it.
    bool wide = false;
    /// HDR is on. DXGI's colour space says this too and this says it earlier,
    /// from the same call as the rest.
    bool hdr = false;
};

AdvancedColour advanced_colour_of(HWND window)
{
    AdvancedColour out;
    if (window == nullptr) {
        return out;
    }
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == 0) {
        return out;
    }

    UINT32 paths = 0;
    UINT32 modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> path_list(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode_list(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path_list.data(), &modes,
                           mode_list.data(), nullptr) != ERROR_SUCCESS) {
        return out;
    }
    path_list.resize(paths);

    for (const DISPLAYCONFIG_PATH_INFO& path : path_list) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        // The GDI device name is what a monitor handle and a display path have
        // in common. Everything else about them is two different vocabularies
        // for the same hardware.
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        // **1000 is the unit, and it is not nits.** The field is the SDR white
        // level in units of 1/1000 of 80 nits, so 1000 means 80 -- the scRGB
        // reference, a scale of exactly one.
        //
        // **And 1000 is what an SDR display reports**, whatever its panel can
        // do. Measured here: a monitor whose peak `IDXGIOutput6` gives as 470
        // nits reports an SDR white level of exactly 1000, because with HDR off
        // Windows composites to the reference and the panel's brightness is the
        // panel's business. The number moves when HDR is on and the user drags
        // the SDR brightness slider, which is the case §9.6 exists for.
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
            white.SDRWhiteLevel != 0) {
            out.sdr_white_nits = static_cast<double>(white.SDRWhiteLevel) * 80.0 / 1000.0;
        }

        // **The one §9.4 said could not be told.** `IDXGIOutput6` reports
        // `G22_NONE_P709` for a wide-gamut display doing its own colour
        // management and for a plain one alike, and that note concluded the
        // difference only exists from Windows 11 24H2's ADVANCED_COLOR_INFO_2.
        // It does not: the original ADVANCED_COLOR_INFO answers it, and the
        // walk for the white level is already standing in front of it.
        // Measured on this machine -- supported 1, enabled 0, wideColorEnforced
        // 1, which is exactly the state that was supposed to be invisible.
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO advanced{};
        advanced.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        advanced.header.size = sizeof(advanced);
        advanced.header.adapterId = path.targetInfo.adapterId;
        advanced.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&advanced.header) == ERROR_SUCCESS) {
            out.hdr = advanced.advancedColorEnabled != 0;
            out.wide = advanced.advancedColorEnabled == 0 &&
                       advanced.wideColorEnforced != 0;
        }
        return out;
    }
    return out;
}

/// What §9.4 could work out, given a device and a window. Kept separate from
/// the plan so the plan stays testable.
mp::video::Display probe_display(IDXGIFactory2* factory, HWND window)
{
    mp::video::Display display{};
    if (factory == nullptr) {
        return display;
    }

    Com<IDXGIOutput6> output6;
    if (!output_for(factory, window, output6)) {
        return display;
    }

    DXGI_OUTPUT_DESC1 desc{};
    if (FAILED(output6->GetDesc1(&desc))) {
        return display;
    }
    // §9.4's caveat, worth keeping in the code that relies on it: this cannot
    // tell an auto-colour-managed SDR display from a plain one. Both report
    // G22_NONE_P709, so `wide` stays false and only Windows 11 24H2's
    // ADVANCED_COLOR_INFO_2 could say otherwise.
    display.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    // **HLG needs this and PQ does not.** PQ states absolute nits; HLG is
    // scene-referred and its OOTF is a system gamma derived from the display's
    // peak, so the same signal is a different picture on two displays and that
    // is the format working as designed rather than a fault.
    if (desc.MaxLuminance > 0.0f) {
        display.peak_nits = desc.MaxLuminance;
    }

    // Asked of the monitor the window is on, which is why it comes last and
    // why it takes the window rather than the output: DXGI knows neither where
    // the user put the SDR slider nor whether a wide-gamut display is managing
    // its own colour.
    const AdvancedColour advanced = advanced_colour_of(window);
    display.sdr_white_nits = advanced.sdr_white_nits;
    display.wide = advanced.wide;
    // Either source will do for this one and they agree; DXGI's answers for a
    // window that is not on any path, and the CCD's for a driver that reports a
    // colour space it is not in.
    display.hdr = display.hdr || advanced.hdr;
    return display;
}

/// **The chroma reconstruction's weights, and what they were worked out
/// for**: the kernel and its parameters, the siting and which axes are
/// subsampled. `first` and `taps` are what the constants say about the four
/// runs in the buffer (see `chroma_weights` in the shader).
struct ChromaTable {
    mp::kernels::KernelParams kernel{};
    double off_x = 0.0;
    double off_y = 0.0;
    bool across = false;
    bool down = false;
    bool built = false;
    std::int32_t first[4] = {0, 0, 0, 0};
    std::uint32_t taps = 8;
};

} // namespace

struct MpVideo {
    ~MpVideo()
    {
        // **A handle this process made is a handle this process closes.** A
        // shell duplicates it into its own; the duplicate is the shell's to
        // close and this one is ours, and leaking it would keep a composition
        // surface alive after the presenter that owned it is gone.
        if (composition != nullptr) {
            ::CloseHandle(composition);
        }
        // The compositor's event is one of ours too, for the same reason.
        if (waitable != nullptr) {
            ::CloseHandle(waitable);
        }
    }

    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGIFactory2> factory;
    Com<IDXGISwapChain1> swap_chain;

    /// What is drawn into: the swap chain's back buffer, or a texture of our
    /// own when there is no window. Both are the same format and take the same
    /// path, which is what makes the off-screen one a measurement rather than
    /// an approximation.
    Com<ID3D11Texture2D> target;
    Com<ID3D11RenderTargetView> target_view;
    /// CPU-readable, for `read_back`.
    Com<ID3D11Texture2D> staging;

    /// **What a provider that is not ours reads.** HDR10 as a display would be
    /// sent it -- PQ on BT.2020, ten bits -- written by the same pixel shader
    /// that would otherwise have written scRGB. Null unless `driver` or `d2d`
    /// is the provider in the path, because otherwise nothing reads it.
    /// Direct2D over the same device, for the `d2d` provider and nothing
    /// else. Made once when that provider is asked for, because opening a D2D
    /// device for a player that will never tone-map is a dependency nobody
    /// wanted.
    Com<ID2D1Device> d2d_device;
    Com<ID2D1DeviceContext> d2d_context;

    Com<ID3D11Texture2D> graded_target;
    Com<ID3D11RenderTargetView> graded_view;
    Com<ID3D11ShaderResourceView> graded_source;

    Com<ID3D11VertexShader> vertex_shader;
    /// One per source kind rather than a branch in one shader: a constant that
    /// is the same for every pixel of every frame is not a thing to test at
    /// every pixel of every frame.
    Com<ID3D11PixelShader> pixel_rgba;
    Com<ID3D11PixelShader> pixel_nv12;
    Com<ID3D11PixelShader> pixel_planar;
    /// The horizontal half of the chroma reconstruction (see `ps_chroma_across`).
    Com<ID3D11PixelShader> pixel_chroma_across;
    /// What it writes: the luma's width at the chroma's height, fp32, remade
    /// when the frame's size or its plane arrangement changes.
    Com<ID3D11Texture2D> chroma_wide;
    Com<ID3D11RenderTargetView> chroma_wide_view;
    Com<ID3D11ShaderResourceView> chroma_wide_srv;
    std::uint32_t chroma_wide_width = 0;
    std::uint32_t chroma_wide_height = 0;
    /// The chroma reconstruction's weights, worked out again when the kernel,
    /// the siting or the subsampling changes and made larger when a longer
    /// kernel needs the room; `chroma_weights_room` is how many floats it has.
    Com<ID3D11Buffer> chroma_weights;
    Com<ID3D11ShaderResourceView> chroma_weights_view;
    std::uint32_t chroma_weights_room = 0;
    ChromaTable chroma_table;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11Buffer> constants;

    /// The frame most recently uploaded. `source` is the BGRA8 path; `luma`
    /// and `chroma` are the two planes of NV12 or P010, kept as separate
    /// textures rather than as one `DXGI_FORMAT_NV12` because that format
    /// carries constraints -- even dimensions, device support -- for a
    /// convenience this does not need.
    Com<ID3D11Texture2D> source;
    Com<ID3D11ShaderResourceView> source_view;
    Com<ID3D11Texture2D> luma;
    Com<ID3D11ShaderResourceView> luma_view;
    /// Semi-planar CbCr, interleaved in one two-component texture.
    Com<ID3D11Texture2D> chroma;
    Com<ID3D11ShaderResourceView> chroma_view;
    /// Planar Cb and Cr, a texture each. **Separate members rather than reusing
    /// `chroma`**: the two arrangements declare different texture types in the
    /// shader, and a one-component view bound where a two-component one is
    /// declared reads garbage in its second channel rather than failing.
    Com<ID3D11Texture2D> chroma_u;
    Com<ID3D11ShaderResourceView> chroma_u_view;
    Com<ID3D11Texture2D> chroma_v;
    Com<ID3D11ShaderResourceView> chroma_v_view;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    MpPixelLayout source_layout{};

    /// What a person set, or the defaults: the chroma kernel and its
    /// parameters (kernels.hpp), the siting and what happens past the gamut
    /// (siting.hpp). The picture's own resampling is a stage's (§9.11), and
    /// its keys are the stage's.
    mp::kernels::Kernel chroma_kernel = mp::kernels::Kernel::lanczos;
    std::uint32_t chroma_lobes = 3;
    double chroma_b = 1.0 / 3.0;
    double chroma_c = 1.0 / 3.0;
    static constexpr std::uint32_t k_siting_auto = 0xffffffffu;
    /// A person's `siting`, or `k_siting_auto` for the stream's own.
    std::uint32_t siting_asked = k_siting_auto;
    /// The siting in use for the frame in hand, resolved by `fill_resampling`.
    std::uint32_t siting = mp::video::k_siting_left;
    /// `MpVideoInfo::chroma_siting` as configured: H.273's type plus one.
    std::uint32_t stream_siting = 0;
    mp::video::Gamut gamut = mp::video::Gamut::desaturate;

    HWND window = nullptr;
    /// **§9.7.1: the frame crosses the boundary, not the window.**
    ///
    /// A composition surface handle, made by `DCompositionCreateSurfaceHandle`
    /// and presented into by a swap chain `IDXGIFactoryMedia` builds on it. A
    /// shell in another process calls `CreateSurfaceFromHandle` on the same
    /// handle, puts it in a visual and commits; this process has no window, no
    /// visual tree and no toolkit, which is what makes a headless engine
    /// headless.
    ///
    /// Null unless `surface` was set to `composition`. The two are exclusive:
    /// an HWND chain draws into a window this process owns, which is the thing
    /// §9.7.1 decided against for an engine and is right for the probe.
    HANDLE composition = nullptr;

    /// The colour space `SetColorSpace1` was actually given, or
    /// `DXGI_COLOR_SPACE_RESERVED` when the chain took none.
    ///
    /// **Remembered because DXGI will not say.** A chain answers `GetDesc1`
    /// for its size and format and has no `GetColorSpace1`, and deciding
    /// whether the chain that exists is the chain that would be built needs
    /// all three.
    DXGI_COLOR_SPACE_TYPE chain_space = DXGI_COLOR_SPACE_RESERVED;
    /// **The compositor's own event, made once.** A waitable swap chain hands
    /// one back and the caller owns it -- `GetFrameLatencyWaitableObject`
    /// duplicates on every call, so asking in `describe` leaked a handle per
    /// row a shell read. Made with the chain, reported from here, and closed
    /// with it.
    HANDLE waitable = nullptr;
    bool warp = false;
    /// Off-screen only: single precision unless a caller asks for the format a
    /// display would actually get. A swap chain has no say -- FP16 is the most
    /// DXGI will present.
    bool wide_target = true;
    /// **What the shell asked for, and what the picture is** (§9.7.1).
    ///
    /// `width` and `height` are the target: what is actually rendered into,
    /// and what `read_back` hands over. `picture_*` is the stream's own size
    /// after the container's aspect correction, kept because `native` has to
    /// be able to come back. `asked_*` is zero until a shell says otherwise.
    ///
    /// The alternative §9.7.1 weighed was to render at the picture's size and
    /// let the shell's visual carry a transform, which costs no message and no
    /// resize. This costs one message and keeps the scale **ours**: the
    /// picture is decoded at the source's own size, §9.8.3's chain runs on
    /// it, and a scaler stage in that chain (`vdsp_scale`, §9.11) resamples
    /// it to this size by whatever kernel a person chose, with whatever
    /// parameters. A composition surface's filter is a fixed bilinear, out of
    /// reach, and the HDR10 test patterns showed what a bilinear does to a
    /// checkerboard. With no scaler in the chain the finish pass's own
    /// bilinear fetch is what is left -- a softer picture rather than none.
    ///
    /// **The whole picture is drawn into the whole target.** Where black bars
    /// go, if any, is the shell's -- it has the window and the compositor puts
    /// the surface where it likes, and bars rendered here would be black
    /// pixels a shell composites over its own background. What it needs to
    /// work the fit out is the picture's size, which `describe` reports.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t picture_width = 0;
    std::uint32_t picture_height = 0;
    std::uint32_t asked_width = 0;
    std::uint32_t asked_height = 0;

    mp::video::Stream stream{};
    /// **What the container said, kept whole.** `Stream` is the four numbers
    /// the colour plan turns on; this is the rest, and it exists for the ten
    /// that `SetHDRMetaData` wants. Its `size` is the caller's, not ours, so a
    /// module built against the header before the mastering display was
    /// appended answers `mp_video_has_mastering` with a no rather than with
    /// whatever was on the stack.
    MpVideoInfo graded{};
    /// `MP_VIDEO_FULL_RANGE` from the container. Studio range is the default
    /// and the safe one: treating 16..235 as 0..255 crushes the blacks and
    /// clips the whites, which reads as a contrast setting rather than a bug.
    bool full_range = false;
    mp::video::Display display{};
    /// **What the shell said the display is** (§9.7.1), or nothing.
    ///
    /// `probe_display` takes a window and, given none, falls back to the first
    /// output -- which for a windowless engine is not a fallback but a guess
    /// about which monitor the picture is on, and every §9 decision turns on
    /// it: the tone mapper, the SDR boost, the HLG system gamma, the encoding.
    /// A shell has the window and knows; this is where it says so, and it says
    /// it again whenever its window crosses a monitor or somebody toggles HDR.
    ///
    /// Empty is *ask the operating system*, which is right for a window this
    /// process owns and for the off-screen measurements -- the two cases that
    /// can answer for themselves -- and is the default for that reason.
    bool display_told = false;
    mp::video::Display told_display{};
    mp::video::Plan plan{};
    mp::video::ToneMap preferred = mp::video::ToneMap::shader;
    bool composited = true;
    bool configured = false;
    /// Said once. A provider that is not on this machine is not on it every
    /// frame, and a log line per frame is a log nobody reads.
    bool tone_map_complained = false;

    /// **§9.8.3's chain.** Empty is one pass, and is the default.
    ///
    /// Copied, because a presenter holding a pointer into a host's vector is a
    /// presenter that crashes when the host adds a stage. The stages themselves
    /// belong to whoever opened them and outlive this only by agreement.
    std::vector<MpVideoStage> chain;
    /// **The fp32 linear picture, at the source's own size**
    /// (`make_linear_picture`): what the first pass stops at when a chain is
    /// in the path or the target is another size, and what the chain -- or
    /// the finish pass's fetch -- reads. Made only then: not paying for it is
    /// the whole point of the single pass.
    Com<ID3D11Texture2D> linear_picture;
    Com<ID3D11RenderTargetView> linear_picture_view;
    Com<ID3D11ShaderResourceView> linear_picture_srv;
    std::uint32_t linear_width = 0;
    std::uint32_t linear_height = 0;
    /// What the chain was told it would be given, and what its last stage
    /// said it would produce: what the finish pass draws from, and what the
    /// `scale` row turns on. Zero with no chain.
    std::uint32_t chain_in_width = 0;
    std::uint32_t chain_in_height = 0;
    std::uint32_t chain_out_width = 0;
    std::uint32_t chain_out_height = 0;
    /// A view over whatever the last stage produced. Remade when that texture
    /// changes, which after the first frame it does not.
    Com<ID3D11ShaderResourceView> chain_out_view;
    void* chain_out_texture = nullptr;
    std::uint64_t frames = 0;
    /// Whether the target holds a frame **at its current size**. A resize
    /// leaves a back buffer whose contents are undefined, which is the same
    /// hazard as reading before the first present and wants the same answer.
    bool drawn = false;
    std::uint64_t last_pts = 0;

    std::string trouble;
};

namespace {

/// The largest texture Direct3D 11 will make at feature level 11, which is
/// `D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION`.
///
/// Spelled out rather than taken from the SDK macro so that the number a person
/// sees in an error message and the number this refuses are one thing. §9.8.2
/// measured it: it is also the ceiling the 16K tests stop at.
constexpr unsigned long k_max_dimension = 16384;

/// The device, and the two ways of getting one.
///
/// **It is made for two jobs.** §9.8.1 puts the device here rather than in the
/// decoder -- a presenter is made once and outlives every decoder a playlist
/// goes through -- so a decoder that will be handed this one needs two things
/// the presenting half never asks for: `ID3D11VideoDevice`, which comes of
/// `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, and multithread protection, because
/// Media Foundation decodes on threads of its own. Both are free to the
/// presenting half and neither can be added afterwards.
// --------------------------------------------------------------------------
// The providers that are not ours
// --------------------------------------------------------------------------
//
// Two helpers below are declared here and defined further down, beside the
// swap chain they are otherwise about: what format the target is, and what
// colour space it is in. Moving either up here would put the swap chain's
// arithmetic in the middle of the tone mappers.
DXGI_FORMAT target_format_of(const MpVideo* v) noexcept;
DXGI_COLOR_SPACE_TYPE colour_space_of(const mp::video::Plan& plan) noexcept;
//
// §9.3 keeps four names and §9.2 explains why more than one of them has to
// exist: the OS mapper is free, hardware accelerated, matches every other
// Windows application, and maps the PQ EOTF to a 2.4 gamma rather than to the
// curve Windows uses for SDR everywhere else. Both statements are true and a
// player has to be able to pick.
//
// **All three take the same input.** The pixel shader decodes to light either
// way; when the provider is not ours it stops one step short and writes HDR10
// -- PQ on BT.2020, which is what a display would be sent -- into
// `graded_target`, and the provider maps *that* into the real one. So there is
// one decode in this file rather than three, and the difference between the
// providers is exactly the roll-off, which is the thing being chosen between.

/// Whether this provider does its work in a second pass rather than in the
/// pixel shader.
bool maps_elsewhere(const MpVideo* v) noexcept
{
    return v->plan.tone_mapping && (v->plan.tone_map == mp::video::ToneMap::driver ||
                                    v->plan.tone_map == mp::video::ToneMap::d2d);
}

/// Tells every stage what it will be given and asks what it would like back.
///
/// **`in` is the picture; `out` arrives pre-filled with what this presenter
/// would like to come out** -- the target's size -- and a stage answers with
/// what it will produce, which for a grade is what it was given and for a
/// scaler is what was asked. The next stage is told the answer. What the last
/// one says is what the finish pass reads: at the target's size an exact
/// fetch, at any other the bilinear fallback, and the `scale` row says which.
bool configure_chain(MpVideo* v)
{
    v->chain_in_width = 0;
    v->chain_in_height = 0;
    v->chain_out_width = 0;
    v->chain_out_height = 0;
    if (v->chain.empty()) {
        return true;
    }
    // The frame in hand when there is one, else what the container said: a
    // decoder's coded size can differ, and `present` asks again when it does.
    const std::uint32_t width = v->source_width != 0 ? v->source_width : v->graded.width;
    const std::uint32_t height = v->source_height != 0 ? v->source_height : v->graded.height;
    MpVideoInfo linear{};
    std::memcpy(&linear, &v->graded, std::min<std::size_t>(v->graded.size, sizeof(linear)));
    linear.size = sizeof(linear);
    linear.width = width;
    linear.height = height;
    if (width != v->graded.width || height != v->graded.height) {
        linear.display_width = width;
        linear.display_height = height;
    }
    // 8 is ITU-T H.273's "linear transfer characteristics". The primaries stay
    // the source's, which is where the pass above leaves the picture.
    linear.transfer = 8;

    for (const MpVideoStage& stage : v->chain) {
        if (stage.vtbl->configure == nullptr) {
            continue;
        }
        MpVideoInfo answered = linear;
        answered.size = sizeof(answered);
        answered.width = v->width;
        answered.height = v->height;
        answered.display_width = v->width;
        answered.display_height = v->height;
        if (stage.vtbl->configure(stage.handle, &linear, &answered) != MP_OK) {
            v->trouble = "a stage would not take this picture";
            return false;
        }
        if (answered.width == 0 || answered.height == 0) {
            v->trouble = "a stage in the chain answered a size of nothing";
            return false;
        }
        answered.size = sizeof(answered);
        linear = answered;
    }
    v->chain_in_width = width;
    v->chain_in_height = height;
    v->chain_out_width = linear.width;
    v->chain_out_height = linear.height;
    return true;
}

/// The fp32 linear picture the chain and the finish pass read, at the
/// source's own size: made when a frame first needs it and remade when the
/// frame's size changes.
///
/// **Single precision and not half**, whatever the swap chain is: this is where
/// a grade happens, and §9.10's argument about precision before the last step
/// applies most exactly to the step that has arithmetic in it.
bool make_linear_picture(MpVideo* v, std::string& why)
{
    if (v->linear_picture && v->linear_width == v->source_width &&
        v->linear_height == v->source_height) {
        return true;
    }
    v->linear_picture_srv.reset();
    v->linear_picture_view.reset();
    v->linear_picture.reset();
    v->chain_out_view.reset();
    v->chain_out_texture = nullptr;
    v->linear_width = 0;
    v->linear_height = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = v->source_width;
    desc.Height = v->source_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(v->device->CreateTexture2D(&desc, nullptr, v->linear_picture.put()))) {
        why = "no linear intermediate, so there is nowhere for a stage to read from";
        return false;
    }
    if (FAILED(v->device->CreateRenderTargetView(v->linear_picture.get(), nullptr,
                                                 v->linear_picture_view.put()))) {
        why = "the linear intermediate would take no render target view";
        return false;
    }
    if (FAILED(v->device->CreateShaderResourceView(v->linear_picture.get(), nullptr,
                                                   v->linear_picture_srv.put()))) {
        why = "the linear intermediate would take no shader resource view";
        return false;
    }
    v->linear_width = v->source_width;
    v->linear_height = v->source_height;
    return true;
}

/// Every stage in turn, and the texture the last one produced.
///
/// **The presenter drives this** (§9.8.3) rather than something between it and
/// the decoder, because a decoder's frame is Y'CbCr in the container's transfer
/// and a stage that converted it would be the second implementation of §9's
/// colour pipeline. Here the conversion has already happened, once, in the pass
/// above.
bool run_chain(MpVideo* v, const MpVideoFrame& source, MpVideoFrame& out)
{
    out = source;
    for (const MpVideoStage& stage : v->chain) {
        if (stage.vtbl == nullptr || stage.handle == nullptr ||
            stage.vtbl->size < sizeof(MpVideoDspVtbl) || stage.vtbl->process == nullptr) {
            continue;
        }
        MpVideoFrame next{};
        next.size = sizeof(next);
        const MpVideoFrame given = out;
        if (stage.vtbl->process(stage.handle, &given, &next) != MP_OK ||
            next.texture == nullptr) {
            // **A stage that refuses does not stop the picture.** The frame it
            // was given goes on to the next one, and the run says so once --
            // §9.1's argument again: a picture that stops is worse than a
            // picture that is not graded.
            if (!v->tone_map_complained) {
                v->tone_map_complained = true;
                v->trouble = "a stage in the chain refused a frame; showing it ungraded";
                log_line(MP_LOG_WARN, ("video_d3d11: " + v->trouble).c_str());
            }
            continue;
        }
        out = next;
    }
    return out.texture != nullptr;
}

/// The HDR10 texture the second pass reads, made only when there is one.
bool make_graded_target(MpVideo* v, std::string& why)
{
    v->graded_source.reset();
    v->graded_view.reset();
    v->graded_target.reset();
    if (!maps_elsewhere(v)) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = v->width;
    desc.Height = v->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // **Ten bits, not sixteen.** This is HDR10 as a display takes it, and the
    // video processor's input views are for formats a display pipeline handles.
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(v->device->CreateTexture2D(&desc, nullptr, v->graded_target.put()))) {
        why = "no HDR10 intermediate, so nothing could tone-map it";
        return false;
    }
    if (FAILED(v->device->CreateRenderTargetView(v->graded_target.get(), nullptr,
                                                 v->graded_view.put())) ||
        FAILED(v->device->CreateShaderResourceView(v->graded_target.get(), nullptr,
                                                   v->graded_source.put()))) {
        why = "the HDR10 intermediate would take no views";
        return false;
    }
    return true;
}

/// **`driver`: the GPU's fixed-function video processor.**
///
/// The cheapest of the four and the one the OS's own player path uses, which is
/// the answer to *why does this look like Windows*. It is told what the stream
/// is (`SetStreamColorSpace1`), what the display is (`SetOutputColorSpace1`)
/// and what the content was graded on (`SetStreamHDRMetaData`, §9.7.2 step
/// six), and the driver does the rest in fixed function.
///
/// **It can simply not be there.** A video processor is a driver feature and
/// WARP's is not the same one; a device that will not make one says so and the
/// caller falls back rather than presenting an unmapped picture.
bool tone_map_by_driver(MpVideo* v, std::string& why)
{
    Com<ID3D11VideoDevice> video_device;
    Com<ID3D11VideoContext> video_context;
    if (FAILED(v->device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                         std::out_ptr<void*>(video_device))) ||
        FAILED(v->context->QueryInterface(
            __uuidof(ID3D11VideoContext), std::out_ptr<void*>(video_context)))) {
        why = "this device has no video processor";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = v->width;
    content.InputHeight = v->height;
    content.OutputWidth = v->width;
    content.OutputHeight = v->height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    Com<ID3D11VideoProcessorEnumerator> enumerator;
    Com<ID3D11VideoProcessor> processor;
    if (FAILED(video_device->CreateVideoProcessorEnumerator(&content, enumerator.put())) ||
        FAILED(video_device->CreateVideoProcessor(enumerator.get(), 0, processor.put()))) {
        why = "this device would not make a video processor";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorInputView> input;
    if (FAILED(video_device->CreateVideoProcessorInputView(
            v->graded_target.get(), enumerator.get(), &in_desc, input.put()))) {
        why = "the video processor would not read an HDR10 texture";
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorOutputView> output;
    if (FAILED(video_device->CreateVideoProcessorOutputView(
            v->target.get(), enumerator.get(), &out_desc, output.put()))) {
        why = "the video processor would not write this target";
        return false;
    }

    Com<ID3D11VideoContext1> video_context1;
    if (SUCCEEDED(video_context->QueryInterface(
            __uuidof(ID3D11VideoContext1), std::out_ptr<void*>(video_context1)))) {
        // **This is the whole request.** PQ on BT.2020 in, the display's own
        // space out; everything the driver does follows from the pair.
        video_context1->VideoProcessorSetStreamColorSpace1(
            processor.get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        video_context1->VideoProcessorSetOutputColorSpace1(processor.get(),
                                                           colour_space_of(v->plan));
    } else {
        why = "this device's video context is too old to be told a colour space";
        return false;
    }

    Com<ID3D11VideoContext2> video_context2;
    if (SUCCEEDED(video_context->QueryInterface(
            __uuidof(ID3D11VideoContext2), std::out_ptr<void*>(video_context2)))) {
        if (mp_video_has_mastering(&v->graded)) {
            // §9.7.2 step six: what the content was graded on. Ours reads it
            // as the peak its roll-off starts from; the driver reads it as it
            // pleases, which is why the same file looks different under the
            // two.
            DXGI_HDR_METADATA_HDR10 hdr{};
            hdr.RedPrimary[0] = static_cast<UINT16>(v->graded.mastering_primaries_x[0]);
            hdr.RedPrimary[1] = static_cast<UINT16>(v->graded.mastering_primaries_y[0]);
            hdr.GreenPrimary[0] = static_cast<UINT16>(v->graded.mastering_primaries_x[1]);
            hdr.GreenPrimary[1] = static_cast<UINT16>(v->graded.mastering_primaries_y[1]);
            hdr.BluePrimary[0] = static_cast<UINT16>(v->graded.mastering_primaries_x[2]);
            hdr.BluePrimary[1] = static_cast<UINT16>(v->graded.mastering_primaries_y[2]);
            hdr.WhitePoint[0] = static_cast<UINT16>(v->graded.mastering_white_x);
            hdr.WhitePoint[1] = static_cast<UINT16>(v->graded.mastering_white_y);
            hdr.MaxMasteringLuminance = v->graded.mastering_max_luminance;
            hdr.MinMasteringLuminance = v->graded.mastering_min_luminance;
            hdr.MaxContentLightLevel =
                static_cast<UINT16>(v->graded.max_content_light_level);
            hdr.MaxFrameAverageLightLevel =
                static_cast<UINT16>(v->graded.max_frame_average_light_level);
            video_context2->VideoProcessorSetStreamHDRMetaData(
                processor.get(), 0, DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr);
        }
        // What the display can show, which is the other half of the request.
        DXGI_HDR_METADATA_HDR10 out_hdr{};
        // Whole units, as HDR10's metadata has them: the nearest, from the
        // display's figure in double.
        out_hdr.MaxMasteringLuminance =
            static_cast<UINT>(std::llround(v->display.peak_nits * 10000.0));
        out_hdr.MaxContentLightLevel = static_cast<UINT16>(std::lround(v->display.peak_nits));
        video_context2->VideoProcessorSetOutputHDRMetaData(
            processor.get(), DXGI_HDR_METADATA_TYPE_HDR10, sizeof(out_hdr), &out_hdr);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.get();
    if (FAILED(video_context->VideoProcessorBlt(processor.get(), output.get(), 0, 1,
                                                &stream))) {
        why = "the video processor would not run";
        return false;
    }
    return true;
}

/// Direct2D on the device we already have, made the first time `d2d` is asked
/// for. **The D3D11 device must be `BGRA_SUPPORT`** or D2D will not take it,
/// which is a flag on the device rather than anything about the picture.
bool make_d2d(MpVideo* v, std::string& why)
{
    if (v->d2d_context) {
        return true;
    }
    Com<IDXGIDevice> dxgi;
    if (FAILED(v->device->QueryInterface(__uuidof(IDXGIDevice),
                                         std::out_ptr<void*>(dxgi)))) {
        why = "this device is not a DXGI device";
        return false;
    }
    D2D1_CREATION_PROPERTIES props{};
    props.threadingMode = D2D1_THREADING_MODE_SINGLE_THREADED;
    props.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    props.options = D2D1_DEVICE_CONTEXT_OPTIONS_NONE;
    if (FAILED(D2D1CreateDevice(dxgi.get(), props, v->d2d_device.put()))) {
        why = "Direct2D would not open on this device";
        return false;
    }
    if (FAILED(v->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                  v->d2d_context.put()))) {
        why = "Direct2D would not make a context";
        return false;
    }
    return true;
}

/// **`d2d`: the Direct2D HDR tone map effect**, which is the same mapper
/// Windows ships for its own HDR video pipeline.
///
/// Between `driver` and `shader` in every way: it is somebody else's arithmetic
/// like the first and it runs on the 3D pipeline like the second, so it works
/// where there is no video processor -- WARP included, which is the only reason
/// any of this is reachable by a test on a machine with no HDR panel.
bool tone_map_by_d2d(MpVideo* v, std::string& why)
{
    if (!v->d2d_context) {
        why = "Direct2D would not open on this device";
        return false;
    }

    Com<IDXGISurface> in_surface;
    Com<IDXGISurface> out_surface;
    if (FAILED(v->graded_target->QueryInterface(
            __uuidof(IDXGISurface), std::out_ptr<void*>(in_surface))) ||
        FAILED(v->target->QueryInterface(__uuidof(IDXGISurface),
                                         std::out_ptr<void*>(out_surface)))) {
        why = "these textures are not DXGI surfaces";
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 in_props{};
    in_props.pixelFormat = {DXGI_FORMAT_R10G10B10A2_UNORM, D2D1_ALPHA_MODE_IGNORE};
    in_props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    D2D1_BITMAP_PROPERTIES1 out_props{};
    out_props.pixelFormat = {target_format_of(v), D2D1_ALPHA_MODE_IGNORE};
    out_props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    Com<ID2D1Bitmap1> in_bitmap;
    Com<ID2D1Bitmap1> out_bitmap;
    if (FAILED(v->d2d_context->CreateBitmapFromDxgiSurface(in_surface.get(), &in_props,
                                                           in_bitmap.put())) ||
        FAILED(v->d2d_context->CreateBitmapFromDxgiSurface(out_surface.get(), &out_props,
                                                           out_bitmap.put()))) {
        why = "Direct2D would not wrap these textures";
        return false;
    }

    Com<ID2D1Effect> effect;
    if (FAILED(v->d2d_context->CreateEffect(CLSID_D2D1HdrToneMap, effect.put()))) {
        why = "this build of Windows has no HDR tone map effect";
        return false;
    }
    effect->SetInput(0, in_bitmap.get());
    // **The two luminances are the whole of the request**, exactly as they are
    // for the driver: what the content can reach, and what the display can.
    // Worked out in double and handed over as the effect's floats, which are
    // what its properties are.
    const double content = mp_video_has_mastering(&v->graded) &&
                                   v->graded.mastering_max_luminance != 0u
                               ? static_cast<double>(v->graded.mastering_max_luminance) /
                                     10000.0
                               : 10000.0;
    (void)effect->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE,
                           static_cast<float>(content));
    (void)effect->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE,
                           static_cast<float>(v->display.sdr_white_nits));
    (void)effect->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
                           v->display.hdr ? D2D1_HDRTONEMAP_DISPLAY_MODE_HDR
                                          : D2D1_HDRTONEMAP_DISPLAY_MODE_SDR);

    v->d2d_context->SetTarget(out_bitmap.get());
    v->d2d_context->BeginDraw();
    v->d2d_context->DrawImage(effect.get());
    const HRESULT drawn = v->d2d_context->EndDraw();
    v->d2d_context->SetTarget(nullptr);
    if (FAILED(drawn)) {
        why = "the HDR tone map effect would not draw";
        return false;
    }
    return true;
}

/// **What the content was graded on, handed to the display.**
///
/// ST.2086's mastering display and CTA-861.3's light levels, in exactly the
/// units `DXGI_HDR_METADATA_HDR10` takes -- which is why `MpVideoInfo` states
/// them in those units rather than in anything friendlier: a demuxer copies
/// what the file said, this hands over what the API wants, and no number is
/// rounded twice on the way.
///
/// **Only when the stream carried it.** Sending a mastering display nobody
/// stated would be inventing one, and a display told the content was graded at
/// 4000 nits when it was graded at 1000 tone-maps for a picture that does not
/// exist. `mp_video_has_mastering` is the question, asked in one place.
///
/// **And only on an HDR chain.** An SDR swap chain has nowhere to put it, and
/// this tree's own tone mapper does not read it: BT.2390 rolls off towards the
/// display's peak, which it knows, rather than away from the content's, which
/// it would have to be told. That is a difference worth keeping in sight when
/// `driver` arrives, because the driver's mapper *does* read this and will
/// behave differently for the same file.
void set_hdr_metadata(MpVideo* v) noexcept
{
    if (!v->swap_chain || !mp_video_has_mastering(&v->graded)) {
        return;
    }
    if (v->plan.encoding != mp::video::Encoding::pq) {
        return;
    }
    Com<IDXGISwapChain4> chain4;
    if (FAILED(v->swap_chain->QueryInterface(__uuidof(IDXGISwapChain4),
                                             std::out_ptr<void*>(chain4)))) {
        return;
    }

    const MpVideoInfo& in = v->graded;
    DXGI_HDR_METADATA_HDR10 hdr{};
    hdr.RedPrimary[0] = static_cast<UINT16>(in.mastering_primaries_x[0]);
    hdr.RedPrimary[1] = static_cast<UINT16>(in.mastering_primaries_y[0]);
    hdr.GreenPrimary[0] = static_cast<UINT16>(in.mastering_primaries_x[1]);
    hdr.GreenPrimary[1] = static_cast<UINT16>(in.mastering_primaries_y[1]);
    hdr.BluePrimary[0] = static_cast<UINT16>(in.mastering_primaries_x[2]);
    hdr.BluePrimary[1] = static_cast<UINT16>(in.mastering_primaries_y[2]);
    hdr.WhitePoint[0] = static_cast<UINT16>(in.mastering_white_x);
    hdr.WhitePoint[1] = static_cast<UINT16>(in.mastering_white_y);
    hdr.MaxMasteringLuminance = in.mastering_max_luminance;
    hdr.MinMasteringLuminance = in.mastering_min_luminance;
    hdr.MaxContentLightLevel = static_cast<UINT16>(in.max_content_light_level);
    hdr.MaxFrameAverageLightLevel =
        static_cast<UINT16>(in.max_frame_average_light_level);

    // A failure is not fatal and not worth a log line every rebuild: a display
    // that will not take metadata shows the picture without it, which is what
    // every stream that states none gets anyway.
    (void)chain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr);
}

bool make_device(MpVideo* v, std::string& why)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    // Only when the layer is installed; the retry below covers a machine
    // without the Graphics Tools feature, which is most machines.
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    static constexpr D3D_FEATURE_LEVEL k_levels[] = {D3D_FEATURE_LEVEL_11_1,
                                                     D3D_FEATURE_LEVEL_11_0};

    const auto create = [&](D3D_DRIVER_TYPE type, UINT extra) {
        HRESULT hr = ::D3D11CreateDevice(nullptr, type, nullptr, flags | extra, k_levels,
                                         static_cast<UINT>(std::size(k_levels)),
                                         D3D11_SDK_VERSION, v->device.put(), nullptr,
                                         v->context.put());
#ifndef NDEBUG
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = ::D3D11CreateDevice(nullptr, type, nullptr, flags | extra, k_levels,
                                     static_cast<UINT>(std::size(k_levels)),
                                     D3D11_SDK_VERSION, v->device.put(), nullptr,
                                     v->context.put());
        }
#endif
        return hr;
    };

    HRESULT hr = E_FAIL;
    if (!v->warp) {
        // **The video flag is asked for on hardware and never on WARP.** WARP
        // has no video device at all -- no `ID3D11VideoDevice`, no decoder
        // profiles -- and asking it for `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`
        // fails with DXGI_ERROR_UNSUPPORTED rather than handing back a device
        // without it. An adapter with no video engine is the same case one step
        // down, which is what the second attempt is for: such a machine still
        // presents, and its decoder is the software one.
        hr = create(D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
        if (FAILED(hr)) {
            hr = create(D3D_DRIVER_TYPE_HARDWARE, 0);
        }
        if (FAILED(hr)) {
            // **WARP is the answer rather than the consolation.** A machine
            // with no usable adapter -- a CI runner, a remote session -- still
            // renders, and renders the same pixels every time, which is what
            // makes a hash of them worth anything.
            log_line(MP_LOG_INFO, "video_d3d11: no hardware device, using WARP");
            v->warp = true;
        }
    }
    if (v->warp) {
        hr = create(D3D_DRIVER_TYPE_WARP, 0);
    }
    if (FAILED(hr)) {
        char code[64];
        std::snprintf(code, sizeof(code), "no Direct3D 11 device (0x%08lX)",
                      static_cast<unsigned long>(hr));
        why = code;
        return false;
    }

    // **Media Foundation decodes on threads of its own**, and a device shared
    // with it has to serialise its own use or the two race. What comes of not
    // saying so is not an error to catch: it is intermittent corruption, which
    // is the worst kind to go looking for months later. Every device gets it,
    // including WARP, because whether a decoder will be handed this one is not
    // known when it is made.
    Com<ID3D11Multithread> threading;
    if (SUCCEEDED(v->device->QueryInterface(__uuidof(ID3D11Multithread),
                                            std::out_ptr<void*>(threading)))) {
        threading->SetMultithreadProtected(TRUE);
    }

    Com<IDXGIDevice> dxgi;
    if (FAILED(v->device->QueryInterface(__uuidof(IDXGIDevice),
                                         std::out_ptr<void*>(dxgi)))) {
        why = "the device is not a DXGI device, which cannot happen";
        return false;
    }
    Com<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                  std::out_ptr<void*>(v->factory)))) {
        why = "no DXGI factory";
        return false;
    }
    return true;
}

bool make_shaders(MpVideo* v, std::string& why)
{
    if (FAILED(v->device->CreateVertexShader(k_vs_main, sizeof(k_vs_main), nullptr,
                                             v->vertex_shader.put())) ||
        FAILED(v->device->CreatePixelShader(k_ps_rgba, sizeof(k_ps_rgba), nullptr,
                                            v->pixel_rgba.put())) ||
        FAILED(v->device->CreatePixelShader(k_ps_nv12, sizeof(k_ps_nv12), nullptr,
                                            v->pixel_nv12.put())) ||
        FAILED(v->device->CreatePixelShader(k_ps_planar, sizeof(k_ps_planar), nullptr,
                                            v->pixel_planar.put())) ||
        FAILED(v->device->CreatePixelShader(k_ps_chroma_across, sizeof(k_ps_chroma_across),
                                            nullptr, v->pixel_chroma_across.put()))) {
        why = "the colour shader would not load";
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(v->device->CreateSamplerState(&sampler, v->sampler.put()))) {
        why = "no sampler";
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(v->device->CreateBuffer(&buffer, nullptr, v->constants.put()))) {
        why = "no constant buffer";
        return false;
    }
    return true;
}

/// **Where the platform's format list enters, and the only place it does.**
///
/// `colour_plan.hpp` decides what the buffer must *hold*; this decides what
/// Windows will put it in. The widest DXGI presents for a linear buffer is
/// half, which §9.10 records as a real shortfall rather than a sufficiency --
/// and a presenter on another platform maps the same plan differently. A
/// Wayland one has `DRM_FORMAT_ABGR16161616`, sixteen bits of integer, which
/// DXGI has no equivalent of; a Metal one has `rgba16Float` and stops where
/// this does.
DXGI_FORMAT dxgi_format_of(const mp::video::Plan& plan) noexcept
{
    // PQ is packed into ten bits because that is the only thing DXGI offers
    // for it, and it cannot be blended -- which is why the plan only asks for
    // it when nothing is composited over the video.
    if (plan.encoding == mp::video::Encoding::pq && !plan.needs_blending) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

/// What is actually rendered into. The same as the swap chain's when there is
/// one, because that is what a display gets; wider off-screen, because a
/// measurement should not be quantised before it is taken.
/// Whether this presenter draws into a texture of its own rather than into a
/// swap chain somebody will show.
///
/// **No window used to mean off screen, and then §9.7.1 added a third place to
/// draw.** A composition surface has no window either, and it has a swap
/// chain, whose buffers are fp16 because that is the most DXGI will present.
/// Read as off screen, it was given an fp32 staging texture against an fp16
/// back buffer -- which `read_back`'s copy would refuse -- and a `precision`
/// row that said fp32 for a target that was not.
bool off_screen(const MpVideo* v) noexcept
{
    return v->window == nullptr && v->composition == nullptr;
}

DXGI_FORMAT target_format_of(const MpVideo* v) noexcept
{
    if (off_screen(v) && v->wide_target &&
        v->plan.encoding == mp::video::Encoding::linear) {
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    return dxgi_format_of(v->plan);
}

/// What a render target is, said in the ABI's words rather than DXGI's.
///
/// **This function and `dxgi_format_of` are the whole of the translation**, and
/// keeping it to two places is the point of v4: a layout crosses the boundary
/// and a DXGI enumerator never does.
MpPixelLayout layout_of(DXGI_FORMAT f) noexcept
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        constexpr MpPixelLayout out = MP_LAYOUT_RGBA32F;
        return out;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        constexpr MpPixelLayout out = MP_LAYOUT_RGB10A2;
        return out;
    }
    default: {
        constexpr MpPixelLayout out = MP_LAYOUT_RGBA16F;
        return out;
    }
    }
}

/// The texture format for one plane of `components` samples at
/// `container_bits` each. Two components is the interleaved chroma of a
/// semi-planar layout; one is a luma plane or a plane of a planar one.
DXGI_FORMAT plane_format_of(std::uint32_t components, std::uint32_t container_bits) noexcept
{
    if (container_bits == 16u) {
        return components == 2u ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
    }
    return components == 2u ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
}

bool same_layout(const MpPixelLayout& a, const MpPixelLayout& b) noexcept
{
    return a.chroma == b.chroma && a.packing == b.packing && a.bits == b.bits &&
           a.container_bits == b.container_bits && a.shift == b.shift &&
           a.flags == b.flags;
}

/// **What this presenter can draw, said once and in the layout's own terms.**
///
/// v3 asked "is it NV12, P010 or BGRA8", which named three combinations and
/// could not describe the fourth. This asks about the properties the shader
/// actually depends on, so what is refused is refused for a reason a person
/// can act on -- and so that the cases which cost nothing to allow are
/// allowed. A 4:2:2 or 4:4:4 semi-planar frame is one of those: the chroma
/// plane's size comes out of `mp_pixel_chroma_width`, the shader samples it
/// with normalised coordinates, and neither knows the difference.
bool presentable(const MpPixelLayout& l, std::string& why)
{
    if (l.bits == 0u || l.container_bits == 0u) {
        why = "a frame that does not say what its pixels are";
        return false;
    }
    if (l.chroma == MP_CHROMA_RGB) {
        constexpr MpPixelLayout bgra8 = MP_LAYOUT_BGRA8;
        if (!same_layout(l, bgra8)) {
            why = "the only RGB frame this presenter takes is 8-bit BGRA";
            return false;
        }
        return true;
    }
    if (l.packing == MP_PACK_INTERLEAVED) {
        // Packed Y'CbCr -- YUY2, AYUV, Y410 and their neighbours -- is a
        // different unpack in the shader and nothing in this tree produces it.
        why = "this presenter takes planar and semi-planar chroma, not interleaved";
        return false;
    }
    if (l.container_bits != 8u && l.container_bits != 16u) {
        why = "a semi-planar frame must store its samples in 8 or 16 bits";
        return false;
    }
    if (l.container_bits == 8u && l.bits != 8u) {
        why = "8-bit samples cannot hold more than 8 bits";
        return false;
    }
    if (l.bits + l.shift > l.container_bits) {
        why = "the frame's significant bits do not fit in the container it names";
        return false;
    }
    return true;
}

DXGI_COLOR_SPACE_TYPE colour_space_of(const mp::video::Plan& plan) noexcept
{
    // scRGB is linear with the BT.709 primaries; HDR10 is PQ with BT.2020.
    return dxgi_format_of(plan) == DXGI_FORMAT_R10G10B10A2_UNORM
               ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
               : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

/// One composition surface handle, made once and kept for this presenter's
/// life.
///
/// **`DCompositionCreateSurfaceHandle` is a free function and needs no
/// composition device**, which is the whole reason §9.7.1 chose this over the
/// alternatives: the engine links one entry point from `dcomp.dll` and builds
/// no visual tree. The visual, the target and the `Commit` are the shell's.
bool make_composition_handle(MpVideo* v, std::string& why)
{
    if (v->composition != nullptr) {
        return true;
    }
    // COMPOSITIONOBJECT | ALL_ACCESS is what a surface handle is opened with;
    // the shell duplicates it into its own process and asks DirectComposition
    // for a surface over it.
    const HRESULT hr = ::DCompositionCreateSurfaceHandle(
        COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &v->composition);
    if (FAILED(hr) || v->composition == nullptr) {
        why = "DirectComposition would not make a surface handle";
        v->composition = nullptr;
        return false;
    }
    return true;
}

/// The views over whatever `target` is now: the one everything draws through,
/// and the staging texture `read_back` copies into.
///
/// Split out from `make_target` because a resize keeps the chain and replaces
/// only these.
bool make_target_views(MpVideo* v, std::string& why)
{
    v->target_view.reset();
    v->staging.reset();

    if (FAILED(v->device->CreateRenderTargetView(v->target.get(), nullptr,
                                                 v->target_view.put()))) {
        why = "no render target view";
        return false;
    }

    // **`read_back` is the point, so the staging texture is not optional.** It
    // is what turns every decision in colour_plan.hpp into pixels somebody can
    // hash, on the same path the display gets.
    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = v->width;
    staging.Height = v->height;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Format = target_format_of(v);
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(v->device->CreateTexture2D(&staging, nullptr, v->staging.put()))) {
        why = "no staging texture, so nothing could be read back";
        return false;
    }
    return true;
}

/// Whether what is already here is what `make_target` would build.
///
/// **A track boundary is `configure` again with the same answer.** The picture
/// is reconfigured for the next file and, for a playlist of one camera's
/// output, nothing it decides has changed. Building the chain again anyway
/// costs a visible frame -- the surface a shell is composing loses its buffers
/// and has nothing to show until the next present -- and costs the frame clock
/// with it, since the waitable object belongs to the chain.
bool target_is_current(MpVideo* v)
{
    if (!v->target || !v->target_view || !v->staging) {
        return false;
    }
    D3D11_TEXTURE2D_DESC has{};
    v->target->GetDesc(&has);
    if (has.Width != v->width || has.Height != v->height) {
        return false;
    }
    if (!v->swap_chain) {
        // Off screen, where the target is a texture of this module's own and
        // `target_format_of` is the whole of what it must match.
        return off_screen(v) && has.Format == target_format_of(v);
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(v->swap_chain->GetDesc1(&desc))) {
        return false;
    }
    return desc.Width == v->width && desc.Height == v->height &&
           desc.Format == dxgi_format_of(v->plan) &&
           v->chain_space == colour_space_of(v->plan);
}

/// The swap chain, or the texture that stands in for one.
bool make_target(MpVideo* v, std::string& why)
{
    if (target_is_current(v)) {
        // **The metadata still, because that is the part that changes.** A new
        // track may be graded on a different mastering display even when every
        // pixel format is the same, and it is a call on the chain rather than a
        // property of it.
        set_hdr_metadata(v);
        return true;
    }
    v->chain_space = DXGI_COLOR_SPACE_RESERVED;
    v->target_view.reset();
    v->target.reset();
    v->staging.reset();
    v->swap_chain.reset();
    if (v->waitable != nullptr) {
        CloseHandle(v->waitable);
        v->waitable = nullptr;
    }
    v->drawn = false;

    const DXGI_FORMAT format = target_format_of(v);

    if (v->composition != nullptr) {
        // **The same chain a window would get, minus the window.** Flip model
        // is not optional here either -- a composition surface takes nothing
        // else -- and the format and colour space are decided exactly as they
        // are for an HWND, because what a shell composites is what a display
        // would have been sent.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = v->width;
        desc.Height = v->height;
        desc.Format = dxgi_format_of(v->plan);
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        // **Premultiplied, not ignored.** A shell composites this over whatever
        // it is drawing; an opaque surface would be right for a full-window
        // video and wrong the moment anything is behind it, and the shell
        // cannot change its mind afterwards.
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        // **The frame clock, for an engine that has no window to wait on.**
        // `show` paces on `WaitForVBlank` against the output its window is on;
        // an engine has neither. A waitable chain hands back an event the
        // compositor sets, which is the same question -- *when may I draw the
        // next one* -- answered by the thing that will actually show it.
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        Com<IDXGIFactoryMedia> media;
        if (FAILED(v->factory->QueryInterface(__uuidof(IDXGIFactoryMedia),
                                              std::out_ptr<void*>(media)))) {
            why = "this DXGI has no factory for composition surfaces";
            return false;
        }
        if (FAILED(media->CreateSwapChainForCompositionSurfaceHandle(
                v->device.get(), v->composition, &desc, nullptr, v->swap_chain.put()))) {
            why = "the composition surface would not take a swap chain";
            return false;
        }

        Com<IDXGISwapChain3> chain3;
        if (SUCCEEDED(v->swap_chain->QueryInterface(
                __uuidof(IDXGISwapChain3), std::out_ptr<void*>(chain3)))) {
            const DXGI_COLOR_SPACE_TYPE space = colour_space_of(v->plan);
            UINT support = 0;
            if (SUCCEEDED(chain3->CheckColorSpaceSupport(space, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0 &&
                SUCCEEDED(chain3->SetColorSpace1(space))) {
                v->chain_space = space;
            }
        }
        set_hdr_metadata(v);

        // **The frame clock, taken once.** §9.7.1: an engine with no window
        // has no vertical blank to wait on, and this event is what the
        // compositor sets instead. Whoever drives the loop waits on it.
        Com<IDXGISwapChain2> chain2;
        if (SUCCEEDED(v->swap_chain->QueryInterface(
                __uuidof(IDXGISwapChain2), std::out_ptr<void*>(chain2)))) {
            v->waitable = chain2->GetFrameLatencyWaitableObject();
        }

        if (FAILED(v->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                            std::out_ptr<void*>(v->target)))) {
            why = "no back buffer on the composition chain";
            return false;
        }
    } else if (v->window != nullptr) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = v->width;
        desc.Height = v->height;
        // The swap chain gets what DXGI will present, which is never the wide
        // one -- `target_format_of` only widens when there is no window.
        desc.Format = dxgi_format_of(v->plan);
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        // **Flip model, always** (§9.5). It is not a preference: a blit-model
        // chain is not eligible for Advanced Color processing, so the older
        // models cannot do the one thing this module exists for.
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(v->factory->CreateSwapChainForHwnd(v->device.get(), v->window, &desc,
                                                      nullptr, nullptr,
                                                      v->swap_chain.put()))) {
            why = "the window would not take a flip-model swap chain";
            return false;
        }

        Com<IDXGISwapChain3> chain3;
        if (SUCCEEDED(v->swap_chain->QueryInterface(
                __uuidof(IDXGISwapChain3), std::out_ptr<void*>(chain3)))) {
            const DXGI_COLOR_SPACE_TYPE space = colour_space_of(v->plan);
            UINT support = 0;
            if (SUCCEEDED(chain3->CheckColorSpaceSupport(space, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0 &&
                SUCCEEDED(chain3->SetColorSpace1(space))) {
                v->chain_space = space;
            }
        }
        set_hdr_metadata(v);
        if (FAILED(v->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                            std::out_ptr<void*>(v->target)))) {
            why = "no back buffer";
            return false;
        }
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = v->width;
        desc.Height = v->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(v->device->CreateTexture2D(&desc, nullptr, v->target.put()))) {
            why = "no off-screen render target";
            return false;
        }
    }

    return make_target_views(v, why);
}

/// **The display changed under a presenter that is already running.**
///
/// A window crossed a monitor, or somebody toggled HDR. Every §9 decision turns
/// on what the display is -- the tone mapper, the SDR boost, the HLG system
/// gamma, and the swap chain's own format -- so this is `plan_for` again and
/// the target rebuilt from its answer. More than a resize costs, and rarer:
/// a window is dragged forty times a second and crosses a monitor once.
///
/// The swap chain goes with it, because the format may have. `ResizeBuffers`
/// can change a format, but not the colour space it was created against, and a
/// chain built for scRGB fp16 is not the chain HDR10 wants.
bool replan(MpVideo* v)
{
    v->display = v->display_told ? v->told_display
                                 : probe_display(v->factory.get(), v->window);
    v->plan = mp::video::plan_for(v->stream, v->display, v->preferred, v->composited);
    if (!make_target(v, v->trouble) || !make_graded_target(v, v->trouble)) {
        return false;
    }
    // A provider that is not ours needs its own device, and which provider the
    // plan wants may have just changed.
    if (maps_elsewhere(v) && v->plan.tone_map == mp::video::ToneMap::d2d && !v->d2d_device &&
        !make_d2d(v, v->trouble)) {
        return false;
    }
    return true;
}

/// **The chroma's half-way texture**: the luma's width at the chroma's height,
/// two channels of fp32, for the frame in hand. Null for a frame with no
/// chroma to reconstruct.
bool make_chroma_wide(MpVideo* v, std::uint32_t width, std::uint32_t height, std::string& why)
{
    if (v->chroma_wide && v->chroma_wide_width == width && v->chroma_wide_height == height) {
        return true;
    }
    v->chroma_wide_srv.reset();
    v->chroma_wide_view.reset();
    v->chroma_wide.reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(v->device->CreateTexture2D(&desc, nullptr, v->chroma_wide.put())) ||
        FAILED(v->device->CreateRenderTargetView(v->chroma_wide.get(), nullptr,
                                                 v->chroma_wide_view.put())) ||
        FAILED(v->device->CreateShaderResourceView(v->chroma_wide.get(), nullptr,
                                                   v->chroma_wide_srv.put()))) {
        why = "no intermediate for the chroma, so it cannot be reconstructed";
        return false;
    }
    v->chroma_wide_width = width;
    v->chroma_wide_height = height;
    return true;
}

/// Whether the frame in hand is resampled on its way to the target: the
/// target is the shell's box or the picture's own size, the source is the
/// frame, and equal means one pass with every sample read where it is.
[[nodiscard]] bool resampling(const MpVideo* v) noexcept
{
    return v->source_width != 0 &&
           (v->width != v->source_width || v->height != v->source_height);
}

/// **The chroma reconstruction's weights, worked out when what they depend on
/// changed.** For each subsampled axis, the run at an even luma sample and
/// the run at an odd one: the kernel sits (phase - offset) / 2 chroma samples
/// past the chroma sample at half the luma sample's position, and `taps_at`
/// gives the samples it reaches and their weights, in double. Zero weights at
/// either end are left off -- at a phase that lands on a chroma sample an
/// interpolating kernel is that one sample -- and every run is padded with
/// zeros to the longest, rounded up to eight, which is the loop the shader
/// unrolls.
bool update_chroma_table(MpVideo* v, double off_x, double off_y, bool across, bool down)
{
    const mp::kernels::KernelParams kernel{v->chroma_kernel, v->chroma_lobes, v->chroma_b,
                                           v->chroma_c};
    ChromaTable& table = v->chroma_table;
    if (table.built && table.kernel.kind == kernel.kind && table.kernel.lobes == kernel.lobes &&
        table.kernel.b == kernel.b && table.kernel.c == kernel.c && table.off_x == off_x &&
        table.off_y == off_y && table.across == across && table.down == down) {
        return true;
    }
    std::vector<mp::kernels::Taps> runs(4);
    const double offsets[2] = {off_x, off_y};
    const bool subsampled[2] = {across, down};
    std::size_t longest = 1;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (!subsampled[axis]) {
            continue;
        }
        for (std::size_t phase = 0; phase < 2; ++phase) {
            const double centre = (static_cast<double>(phase) - offsets[axis]) * 0.5;
            mp::kernels::Taps taps = mp::kernels::taps_at(kernel, centre, 1.0);
            std::size_t lead = 0;
            std::size_t end = taps.weights.size();
            while (lead < end && taps.weights[lead] == 0.0) {
                ++lead;
            }
            while (end > lead && taps.weights[end - 1] == 0.0) {
                --end;
            }
            taps.first += static_cast<std::int64_t>(lead);
            taps.weights.assign(taps.weights.begin() + static_cast<std::ptrdiff_t>(lead),
                                taps.weights.begin() + static_cast<std::ptrdiff_t>(end));
            longest = std::max(longest, taps.weights.size());
            runs[(axis * 2) + phase] = std::move(taps);
        }
    }
    const std::size_t stride = (longest + 7) / 8 * 8;
    std::vector<float> weights(4 * stride, 0.0f);
    for (std::size_t run = 0; run < 4; ++run) {
        mp::kernels::round_run(runs[run].weights, weights.data() + (run * stride));
        table.first[run] = static_cast<std::int32_t>(runs[run].first);
    }

    const auto room = static_cast<std::uint32_t>(weights.size());
    if (!v->chroma_weights || v->chroma_weights_room < room) {
        v->chroma_weights_view.reset();
        v->chroma_weights.reset();
        v->chroma_weights_room = 0;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = room * static_cast<std::uint32_t>(sizeof(float));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(float);
        const D3D11_SUBRESOURCE_DATA initial{weights.data(), 0, 0};
        if (FAILED(v->device->CreateBuffer(&desc, &initial, v->chroma_weights.put())) ||
            FAILED(v->device->CreateShaderResourceView(v->chroma_weights.get(), nullptr,
                                                       v->chroma_weights_view.put()))) {
            v->trouble = "no buffer for the chroma reconstruction's weights";
            return false;
        }
        v->chroma_weights_room = room;
    } else {
        const D3D11_BOX box{0, 0, 0, room * static_cast<std::uint32_t>(sizeof(float)), 1, 1};
        v->context->UpdateSubresource(v->chroma_weights.get(), 0, &box, weights.data(), 0, 0);
    }
    table.kernel = kernel;
    table.off_x = off_x;
    table.off_y = off_y;
    table.across = across;
    table.down = down;
    table.taps = static_cast<std::uint32_t>(stride);
    table.built = true;
    return true;
}

/// **What the fetch needs to know about chroma, and which kernels.** Where a
/// chroma sample sits is H.273's table (kernels.hpp), applied per axis by the
/// layout's subsampling: 4:2:2 has no vertical siting and 4:4:4 has none at
/// all. A person's `siting` wins; then the stream's own statement; then type
/// 0, which is what MPEG-2, H.264, HEVC and AV1 mean by silence.
bool fill_resampling(MpVideo* v, Constants& constants)
{
    const MpPixelLayout& layout = v->source_layout;
    const bool subsampled_x = layout.chroma == MP_CHROMA_420 || layout.chroma == MP_CHROMA_422;
    const bool subsampled_y = layout.chroma == MP_CHROMA_420;
    const std::uint32_t type = v->siting_asked < mp::video::k_siting_types ? v->siting_asked
                               : v->stream_siting != 0 && v->stream_siting <= mp::video::k_siting_types
                                   ? v->stream_siting - 1u
                                   : mp::video::k_siting_left;
    v->siting = type;
    const double off_x = subsampled_x ? mp::video::siting_offset_x(type) : 0.0;
    const double off_y = subsampled_y ? mp::video::siting_offset_y(type) : 0.0;
    if (!update_chroma_table(v, off_x, off_y, subsampled_x, subsampled_y)) {
        return false;
    }
    constants.chroma_step[0] = subsampled_x ? 0.5f : 1.0f;
    constants.chroma_step[1] = subsampled_y ? 0.5f : 1.0f;
    const bool has_chroma = layout.chroma != MP_CHROMA_RGB && layout.chroma != MP_CHROMA_MONO;
    const std::uint32_t chroma_width =
        has_chroma ? mp_pixel_chroma_width(&layout, v->source_width) : 1u;
    const std::uint32_t chroma_height =
        has_chroma ? mp_pixel_chroma_height(&layout, v->source_height) : 1u;
    constants.chroma_size[0] = static_cast<float>(chroma_width != 0 ? chroma_width : 1u);
    constants.chroma_size[1] = static_cast<float>(chroma_height != 0 ? chroma_height : 1u);
    for (std::size_t run = 0; run < 4; ++run) {
        constants.chroma_first[run] = v->chroma_table.first[run];
    }
    constants.chroma_taps = v->chroma_table.taps;
    constants.gamut_mode = static_cast<std::uint32_t>(v->gamut);
    return true;
}

/// **Where the roll-off starts from**, out of what the stream stated. The
/// mastering display's peak (ST.2086, in ten-thousandths of a nit) is the
/// grade's range; the content light level (CTA-861.3, in nits) is the
/// brightest pixel actually in it, and when it is stated and lower it is the
/// tighter bound: a 4000-nit mastering display with nothing above 1000 on it
/// is a 1000-nit picture, and rolling it off from 4000 spends the display's
/// range on highlights that never come. Measured on a set of test patterns
/// whose every combination of the two is a separate folder. Zero when neither
/// is stated, and the plan then assumes BT.2408's 1000.
[[nodiscard]] double source_peak_of(const MpVideoInfo* in) noexcept
{
    if (in->size < offsetof(MpVideoInfo, max_frame_average_light_level) + sizeof(std::uint32_t)) {
        return 0.0;
    }
    const double mastering = in->mastering_max_luminance != 0
                                 ? static_cast<double>(in->mastering_max_luminance) / 10000.0
                                 : 0.0;
    const auto content = static_cast<double>(in->max_content_light_level);
    if (content > 0.0 && (mastering <= 0.0 || content < mastering)) {
        return content;
    }
    return mastering;
}

/// The target size: what the shell asked for, or the picture's own.
void target_size(const MpVideo* v, std::uint32_t& width, std::uint32_t& height) noexcept
{
    width = v->asked_width != 0 ? v->asked_width : v->picture_width;
    height = v->asked_height != 0 ? v->asked_height : v->picture_height;
}

/// **A new size on a presenter that is already running**, which is what a
/// window being dragged by a corner looks like from here.
///
/// A swap chain resizes in place. Building a new one over the same composition
/// surface handle would be a second chain on a surface that already has one,
/// and DXGI is entitled to refuse that; `ResizeBuffers` is the operation this
/// case exists for. Off screen there is no chain and the texture is simply
/// made afresh.
bool resize_target(MpVideo* v)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    target_size(v, width, height);
    if (width == v->width && height == v->height) {
        return true;
    }
    v->width = width;
    v->height = height;
    v->drawn = false;

    // **And the chain**, told the new size to answer to: a scaler stage in it
    // produces the target's size, and the target just changed.
    if (!v->swap_chain) {
        return make_target(v, v->trouble) && make_graded_target(v, v->trouble) &&
               configure_chain(v);
    }

    // **Every reference to the back buffer, gone first.** `ResizeBuffers`
    // fails while one outlives it, and a render target still bound to the
    // pipeline is such a reference -- which is why the unbind and the flush
    // are here rather than left to the next draw.
    ID3D11RenderTargetView* none[] = {nullptr};
    v->context->OMSetRenderTargets(1, none, nullptr);
    v->target_view.reset();
    v->target.reset();
    v->staging.reset();
    v->context->Flush();

    // Its own flags back, not a fresh guess at them: the composition chain is
    // waitable and the window chain is not, and a resize that dropped the flag
    // would take the frame clock with it.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(v->swap_chain->GetDesc1(&desc))) {
        v->trouble = "the swap chain would not say what it is";
        return false;
    }
    if (FAILED(v->swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                            desc.Flags))) {
        v->trouble = "the swap chain would not resize";
        return false;
    }
    if (FAILED(v->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                        std::out_ptr<void*>(v->target)))) {
        v->trouble = "no back buffer after the resize";
        return false;
    }
    return make_target_views(v, v->trouble) && make_graded_target(v, v->trouble) &&
           configure_chain(v);
}

/// A dynamic texture and a view over it, made once and reused while the
/// geometry holds. Everything a frame needs is one of these per plane.
bool make_plane(MpVideo* v, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                Com<ID3D11Texture2D>& texture, Com<ID3D11ShaderResourceView>& view,
                std::string& why)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(v->device->CreateTexture2D(&desc, nullptr, texture.put())) ||
        FAILED(v->device->CreateShaderResourceView(texture.get(), nullptr, view.put()))) {
        why = "no texture for the frame";
        return false;
    }
    return true;
}

/// Rows into a mapped texture, honouring both strides.
bool write_plane(MpVideo* v, ID3D11Texture2D* texture, const void* src,
                 std::uint32_t src_stride, std::uint32_t row_bytes, std::uint32_t rows,
                 std::string& why)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(v->context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        why = "the frame texture would not map";
        return false;
    }
    const auto* from = static_cast<const std::uint8_t*>(src);
    auto* to = static_cast<std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < rows; ++y) {
        std::memcpy(to + static_cast<std::size_t>(y) * mapped.RowPitch,
                    from + static_cast<std::size_t>(y) * src_stride, row_bytes);
    }
    v->context->Unmap(texture, 0);
    return true;
}

/// A decoder's own texture, viewed rather than copied.
///
/// **This is what decoding on the GPU is for**: an `ID3D11VideoDecoder` writes
/// NV12 into an array it owns, and a frame is a slice of that array. Two views
/// over the one texture -- `R8_UNORM` for the luma plane and `R8G8_UNORM` for
/// the interleaved chroma -- is how D3D11 exposes the planes of an NV12
/// resource, and nothing is read back, copied or converted on the way.
///
/// It needs `D3D11_BIND_SHADER_RESOURCE` on the decoder's output, which is not
/// automatic and **is not a matter of luck**: an MFT states whether it is
/// D3D11-aware and takes `MF_SA_D3D11_BINDFLAGS` on its output stream, so a
/// host asks for the binding rather than hoping for it. `codec_mft` asks.
bool adopt(MpVideo* v, const MpVideoFrame& frame, std::string& why)
{
    auto* texture = static_cast<ID3D11Texture2D*>(frame.texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        // A driver that would not give the binding the decoder asked for. The
        // answer is a copy into a texture that allows one, which nothing here
        // needs yet -- and saying which of the two happened is worth more than
        // doing it silently.
        why = "the decoder texture cannot be sampled: it was created without "
              "D3D11_BIND_SHADER_RESOURCE";
        return false;
    }
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) {
        why = "the decoder texture is neither NV12 nor P010";
        return false;
    }
    // **The texture is the authority, not the frame.** A decoder states a
    // layout as well, and where the two disagree the resource is what will
    // actually be sampled -- so this reads the resource and the mismatch, if
    // there is one, shows up as a refusal rather than as a wrong picture.
    if (frame.texture_index >= desc.ArraySize) {
        why = "the frame names a slice past the end of the decoder's array";
        return false;
    }

    // A device of its own is a frame from a decoder somebody opened on another
    // device -- see plan.md §9.8.1. Sharing it would take a shared handle and a
    // fence, and being told is better than the picture being wrong.
    Com<ID3D11Device> owner;
    texture->GetDevice(owner.put());
    if (owner.get() != v->device.get()) {
        why = "the frame is on a different device from the presenter";
        return false;
    }

    constexpr MpPixelLayout k_nv12 = MP_LAYOUT_NV12;
    constexpr MpPixelLayout k_p010 = MP_LAYOUT_P010;
    const MpPixelLayout adopted = desc.Format == DXGI_FORMAT_P010 ? k_p010 : k_nv12;
    const bool ten_bit = adopted.container_bits == 16u;
    v->source_view.reset();
    v->source.reset();

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MostDetailedMip = 0;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.FirstArraySlice = frame.texture_index;
    view.Texture2DArray.ArraySize = 1;

    view.Format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (FAILED(v->device->CreateShaderResourceView(texture, &view, v->luma_view.put()))) {
        why = "no luma view over the decoder texture";
        return false;
    }
    view.Format = ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(v->device->CreateShaderResourceView(texture, &view, v->chroma_view.put()))) {
        why = "no chroma view over the decoder texture";
        return false;
    }

    // The textures this module owns are not in the path for an adopted frame,
    // and holding them would keep a decoder's pool larger than it needs to be.
    v->luma.reset();
    v->chroma.reset();
    v->chroma_u.reset();
    v->chroma_u_view.reset();
    v->chroma_v.reset();
    v->chroma_v_view.reset();
    v->source_width = frame.width != 0 ? frame.width : desc.Width;
    v->source_height = frame.height != 0 ? frame.height : desc.Height;
    v->source_layout = adopted;
    return true;
}

/// The frame, however it arrived.
///
/// **Every dimension here is derived from the layout rather than switched on.**
/// The chroma planes' size, the bytes in a sample, the texture format: three
/// pieces of arithmetic that used to be a pair of ternaries reading
/// `format == MP_PIXEL_P010`, and that now say what they mean for a layout
/// nobody wrote a branch for.
bool upload(MpVideo* v, const MpVideoFrame& frame, std::string& why)
{
    if (frame.texture != nullptr) {
        return adopt(v, frame, why);
    }
    const MpPixelLayout& in = frame.layout;
    if (!presentable(in, why)) {
        return false;
    }
    if (frame.width == 0 || frame.height == 0) {
        why = "a frame with no pixels in it";
        return false;
    }

    const bool changed = v->source_width != frame.width ||
                         v->source_height != frame.height ||
                         !same_layout(v->source_layout, in);
    if (changed) {
        v->source_view.reset();
        v->source.reset();
        v->luma_view.reset();
        v->luma.reset();
        v->chroma_view.reset();
        v->chroma.reset();
        v->chroma_u_view.reset();
        v->chroma_u.reset();
        v->chroma_v_view.reset();
        v->chroma_v.reset();
        v->source_width = frame.width;
        v->source_height = frame.height;
        v->source_layout = in;
    }

    if (in.chroma == MP_CHROMA_RGB) {
        const std::uint32_t row = frame.width * mp_pixel_bytes(&in);
        if (frame.plane[0] == nullptr || frame.stride[0] < row) {
            why = "the frame has no pixels, or a stride too short for its width";
            return false;
        }
        if (!v->source &&
            !make_plane(v, frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                        v->source, v->source_view, why)) {
            return false;
        }
        return write_plane(v, v->source.get(), frame.plane[0], frame.stride[0], row,
                           frame.height, why);
    }

    // Rounded up by `mp_pixel_chroma_width`, because an odd width still has a
    // chroma column for its last pixel -- and the same call answers for 4:2:2
    // and 4:4:4 without being told which.
    const std::uint32_t planes = mp_pixel_planes(&in);
    const std::uint32_t chroma_width = mp_pixel_chroma_width(&in, frame.width);
    const std::uint32_t chroma_height = mp_pixel_chroma_height(&in, frame.height);
    const std::uint32_t sample_bytes = mp_pixel_component_bytes(&in);
    const std::uint32_t luma_row = frame.width * sample_bytes;
    // Two components a sample when they are interleaved, one when they are not.
    const std::uint32_t chroma_row =
        chroma_width * sample_bytes * (planes == 2u ? 2u : 1u);

    if (frame.plane[0] == nullptr || frame.stride[0] < luma_row) {
        why = "a frame with no luma plane, or a stride too short for its width";
        return false;
    }
    for (std::uint32_t i = 1; i < planes; ++i) {
        if (frame.plane[i] == nullptr || frame.stride[i] < chroma_row) {
            why = "a chroma plane is missing, or its stride is too short for its width";
            return false;
        }
    }

    if (!v->luma && !make_plane(v, frame.width, frame.height,
                                plane_format_of(1u, in.container_bits), v->luma,
                                v->luma_view, why)) {
        return false;
    }
    if (!write_plane(v, v->luma.get(), frame.plane[0], frame.stride[0], luma_row,
                     frame.height, why)) {
        return false;
    }
    if (planes == 1u) {
        // 4:0:0. The luma plane is the whole picture and the shader is told so
        // rather than left to sample two textures that were never made.
        return true;
    }
    if (planes == 2u) {
        if (!v->chroma &&
            !make_plane(v, chroma_width, chroma_height,
                        plane_format_of(2u, in.container_bits), v->chroma,
                        v->chroma_view, why)) {
            return false;
        }
        return write_plane(v, v->chroma.get(), frame.plane[1], frame.stride[1],
                           chroma_row, chroma_height, why);
    }

    if (!v->chroma_u &&
        (!make_plane(v, chroma_width, chroma_height,
                     plane_format_of(1u, in.container_bits), v->chroma_u,
                     v->chroma_u_view, why) ||
         !make_plane(v, chroma_width, chroma_height,
                     plane_format_of(1u, in.container_bits), v->chroma_v,
                     v->chroma_v_view, why))) {
        return false;
    }
    return write_plane(v, v->chroma_u.get(), frame.plane[1], frame.stride[1], chroma_row,
                       chroma_height, why) &&
           write_plane(v, v->chroma_v.get(), frame.plane[2], frame.stride[2], chroma_row,
                       chroma_height, why);
}

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

MpResult MP_CALL video_open(void* window, MpVideo** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    auto v = std::unique_ptr<MpVideo>(new (std::nothrow) MpVideo());
    if (v == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    v->window = static_cast<HWND>(window);
    *out = v.release();
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

void MP_CALL video_close(MpVideo* v) noexcept
{
    delete v;
}

MpResult MP_CALL video_configure(MpVideo* v, const MpVideoInfo* in) noexcept
try {
    if (v == nullptr || in == nullptr || in->width == 0 || in->height == 0) {
        return MP_ERR_INVALID;
    }

    // Kept whole, and kept at the caller's own `size`: what was not sent is
    // not there, and a zeroed tail read as a mastering display would be a
    // display told the content was graded at nothing.
    v->graded = MpVideoInfo{};
    std::memcpy(&v->graded, in, std::min<std::size_t>(in->size, sizeof(v->graded)));
    v->graded.size = in->size;

    v->stream = mp::video::Stream{.primaries = in->primaries,
                                  .transfer = in->transfer,
                                  .matrix = in->matrix,
                                  .width = in->width,
                                  .height = in->height,
                                  // ST.2086 states it in ten-thousandths of a nit. **The
                                  // peak on its own counts**: a file may state the
                                  // luminances and no primaries -- Windows' own HDR
                                  // sample does -- and `mp_video_has_mastering`, which
                                  // asks after the white point, rightly says that is
                                  // no mastering display. It is still the peak the
                                  // roll-off should start from.
                                  .mastering_peak_nits = source_peak_of(in)};
    v->full_range = (in->flags & MP_VIDEO_FULL_RANGE) != 0;
    v->stream_siting = mp_video_chroma_siting(in);
    // The container's aspect correction is the picture's size, not the
    // codec's: anamorphic 4:3 in a 16:9 frame is 16:9 the moment it is drawn.
    v->picture_width = in->display_width != 0 ? in->display_width : in->width;
    v->picture_height = in->display_height != 0 ? in->display_height : in->height;
    target_size(v, v->width, v->height);

    if (!v->device && !make_device(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    if (!v->vertex_shader && !make_shaders(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }

    // What the shell said, or what this process can find out. The order is
    // the point: a caller that knows beats a probe that is guessing.
    v->display = v->display_told ? v->told_display
                                 : probe_display(v->factory.get(), v->window);
    v->plan = mp::video::plan_for(v->stream, v->display, v->preferred, v->composited);

    if (!make_target(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    // **Only when a provider that is not ours is in the path**, and Direct2D
    // only when that provider is `d2d`: opening a D2D device for a player that
    // will never tone-map is a dependency nobody asked for.
    if (!make_graded_target(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    if (!configure_chain(v)) {
        return MP_ERR_UNSUPPORTED;
    }
    if (maps_elsewhere(v) && v->plan.tone_map == mp::video::ToneMap::d2d &&
        !make_d2d(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    v->configured = true;
    v->frames = 0;
    v->drawn = false;
    v->trouble.clear();
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL video_present(MpVideo* v, const MpVideoFrame* frame) noexcept
try {
    if (v == nullptr || frame == nullptr) {
        return MP_ERR_INVALID;
    }
    if (!v->configured) {
        v->trouble = "nothing has been configured, so there is nothing to present into";
        return MP_ERR_INVALID;
    }
    if (!upload(v, *frame, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }

    // Which of the three the frame is, asked of the layout each time rather
    // than remembered: a stream that changes shape mid-file changes this with
    // it. 4:0:0 goes down the planar path with one plane and `has_chroma` 0.
    const bool ycbcr = v->source_layout.chroma != MP_CHROMA_RGB;
    const bool semi_planar = ycbcr && mp_pixel_planes(&v->source_layout) == 2u;
    ID3D11PixelShader* shader = v->pixel_rgba.get();
    if (semi_planar) {
        shader = v->pixel_nv12.get();
    } else if (ycbcr) {
        shader = v->pixel_planar.get();
    }

    Constants constants{};
    constants.sdr_scale = static_cast<float>(v->plan.sdr_scale);

    // **The transfer, by the code point the container stated.** sRGB gets the
    // piecewise curve; BT.709 and BT.601 video gets BT.1886, a pure 2.4, which
    // is the reference display EOTF those standards specify and what the
    // picture was graded on. Decoding video with the sRGB curve instead lifts
    // the shadows, which is the mirror image of §9.2's complaint.
    //
    // **And the two HDR curves, which the plan has been deciding about since
    // before the shader could do either.** `Convert` is what says which:
    // `hlg_to_linear` is HLG's inverse OETF *and* its OOTF, and `to_linear`
    // over a PQ stream is ST.2084. A stream reaching a PQ buffer unchanged
    // (`Convert::none`) needs no curve here at all, and the plan only sends PQ
    // there.
    const std::uint32_t transfer = mp::video::assumed_transfer(v->stream);
    constants.transfer_gamma = 2.4f;
    if (v->plan.convert == mp::video::Convert::hlg_to_linear) {
        constants.transfer_kind = 3;
    } else if (transfer == mp::video::k_transfer_pq &&
               v->plan.convert != mp::video::Convert::none) {
        constants.transfer_kind = 2;
    } else if (transfer == mp::video::k_transfer_srgb) {
        constants.transfer_kind = 0;
    } else if (transfer == 8u) {
        constants.transfer_kind = 4; // linear, which H.273 names and a test pattern uses
    } else {
        constants.transfer_kind = 1;
    }
    constants.hlg_peak = static_cast<float>(v->plan.hlg_peak_nits);
    // BT.2100's system gamma for that peak, less one, in double: the same for
    // every pixel, where the shader took a logarithm of it at every one.
    constants.hlg_gamma1 = static_cast<float>(
        0.2 + (0.42 * std::log10(std::max(v->plan.hlg_peak_nits, 1.0) / 1000.0)));

    // **Ours, and only ours.** §9.3 keeps four names and this shader is one of
    // them; `driver` and `d2d` are other people's code on other passes, and a
    // pixel shader that quietly tone-mapped when one of those was chosen would
    // be two mappers in the path. Zero means the shader does nothing, which is
    // also what an HDR display gets.
    constants.tone_peak =
        v->plan.tone_mapping && v->plan.tone_map == mp::video::ToneMap::shader
            ? static_cast<float>(v->plan.tone_target_nits)
            : 0.0f;
    // **The EETF's part that is the same for every pixel**, in double: the
    // shader worked out both peaks' PQ code values, four powers, at every
    // pixel, and the knee from them in single precision.
    {
        const double source = nits_to_pq(v->plan.tone_source_nits);
        const double target = nits_to_pq(v->plan.tone_target_nits);
        const double mt = target / source;
        const double ks = (1.5 * mt) - 0.5;
        constants.tone_source_pq = static_cast<float>(source);
        constants.tone_source_inv = static_cast<float>(1.0 / source);
        constants.tone_mt = static_cast<float>(mt);
        constants.tone_ks = static_cast<float>(ks);
        constants.tone_span_inv = static_cast<float>(1.0 / std::max(1.0 - ks, 1e-6));
        // And the output's unit as a factor: a division at every pixel was
        // two and a half units in the last place, as Direct3D allows it, where
        // this is one rounding here and one there.
        constants.tone_peak_inv = v->plan.tone_target_nits > 0.0
                                      ? static_cast<float>(1.0 / v->plan.tone_target_nits)
                                      : 0.0f;
    }
    // **Stop one step short for the other two.** They map in their own pass and
    // want what a display would be sent, so the shader decodes and then
    // re-encodes as PQ rather than converting to scRGB and moving the gamut.
    constants.emit_pq = maps_elsewhere(v) && v->graded_view ? 1u : 0u;

    // **The gamut, when the buffer's primaries are not the stream's.** HDR is
    // graded on BT.2100, whose primaries are BT.2020's, and scRGB is BT.709: a
    // PQ or HLG stream presented without this is oversaturated in the way that
    // looks like a decision rather than a fault. A PQ buffer is BT.2020 already
    // and needs nothing.
    const std::uint32_t primaries = mp::video::assumed_primaries(v->stream);
    if (primaries == mp::video::k_primaries_bt2020 &&
        v->plan.encoding == mp::video::Encoding::linear) {
        for (std::size_t row = 0; row < 3; ++row) {
            float* into = row == 0 ? constants.gamut0
                                   : (row == 1 ? constants.gamut1 : constants.gamut2);
            for (std::size_t col = 0; col < 3; ++col) {
                into[col] = static_cast<float>(k_bt2020_to_bt709[(row * 3) + col]);
            }
        }
    }
    // The luminance the gamut compression keeps: the buffer's own, from the
    // same derivation as the matrix.
    for (std::size_t c = 0; c < 3; ++c) {
        constants.luma709[c] = static_cast<float>(k_bt709_to_xyz[3 + c]);
    }

    if (ycbcr) {
        // **Matrix code point 0 is identity: the planes are R, G and B in
        // Y'CbCr clothing.** VP9 profiles 1 and 3 carry it as sRGB 4:4:4 and
        // AV1 can say it too. `yuv_matrix_for` has no row for it and falls
        // through to BT.709, which would draw such a stream in confidently
        // wrong colours. Refused with a sentence until somebody brings a
        // fixture and the plane-order swap it needs. Unspecified is 2, so an
        // untagged stream is not caught by this.
        if (v->stream.matrix == 0u && v->source_layout.chroma != MP_CHROMA_MONO) {
            v->trouble = "the stream says its matrix is identity -- RGB planes -- "
                         "which nothing here draws yet";
            return MP_ERR_UNSUPPORTED;
        }
        constants.has_chroma = v->source_layout.chroma == MP_CHROMA_MONO ? 0.0f : 1.0f;
        // Derived in double and rounded once, which is §9.10's rule and this
        // is where it pays: eight coefficients per matrix, all of them ratios
        // of the luma weights.
        // **The scale comes from where the bits sit, not from how many.**
        // Ten bits at the top of sixteen and ten at the bottom are the same
        // depth and a factor of sixty-four apart; `shift` is what separates
        // them and `mp_pixel_sample_scale` is that arithmetic.
        const mp::video::YuvMatrix m = mp::video::yuv_matrix_for(
            v->stream.matrix, v->full_range, mp_pixel_sample_scale(&v->source_layout));
        constants.luma_offset = m.luma_offset;
        constants.luma_scale = m.luma_scale;
        constants.chroma_scale = m.chroma_scale;
        constants.r_v = m.r_v;
        constants.g_u = m.g_u;
        constants.g_v = m.g_v;
        constants.b_u = m.b_u;
        constants.sample_scale = m.sample_scale;
    }

    // **The resampling, and where chroma sits**, read by the fetch in every
    // pass that touches a plane.
    if (!fill_resampling(v, constants)) {
        return MP_ERR_UNSUPPORTED;
    }

    // **Where the first pass lands, and how many there are.** The picture is
    // the target's size and no chain is in the path: one pass, straight into
    // the target -- or into the HDR10 intermediate a provider that is not
    // ours maps from. Otherwise the first pass stops at linear light on the
    // source's own primaries, at the source's own size, so that every sample
    // is read once, exactly where it is; then the chain, if there is one,
    // which is where a scaler stage takes the picture to the target's size
    // (§9.11); and then the second half -- tone mapping, the gamut and the
    // encoding -- from linear, drawn over the whole target from whatever the
    // chain produced: an exact fetch at the target's size, and the sampler's
    // bilinear at any other, which is what a chain with no scaler leaves.
    const bool chained = !v->chain.empty();
    const bool fits = !resampling(v);
    const bool second_pass = maps_elsewhere(v) && v->graded_view;

    const auto upload_constants = [&]() {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(v->context->Map(v->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                      &mapped))) {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            v->context->Unmap(v->constants.get(), 0);
        }
    };
    // **Everything bound, every time, because a stage is a program too.** A
    // stage draws with its own vertex shader, its own sampler and its own
    // constant buffer at the same slots this one uses; whatever ran last left
    // them bound. The first version of the chain rebound only the pixel
    // shader and the texture, and the second pass read the *stage's*
    // constants as though they were the presenter's -- a black picture with
    // nothing reporting an error anywhere. And the previous pass's render
    // target is this pass's source, so it comes off the output before it goes
    // on the input; Direct3D unbinds one of the two silently otherwise.
    const auto draw = [&](ID3D11PixelShader* pixel, ID3D11RenderTargetView* into,
                          std::uint32_t width, std::uint32_t height,
                          ID3D11ShaderResourceView* const (&sources)[7]) {
        const D3D11_VIEWPORT viewport{0.0f,
                                      0.0f,
                                      static_cast<float>(width),
                                      static_cast<float>(height),
                                      0.0f,
                                      1.0f};
        ID3D11RenderTargetView* none[] = {nullptr};
        ID3D11ShaderResourceView* unbound[7] = {nullptr, nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr};
        v->context->OMSetRenderTargets(1, none, nullptr);
        v->context->PSSetShaderResources(0, 7, unbound);
        ID3D11RenderTargetView* views[] = {into};
        ID3D11SamplerState* samplers[] = {v->sampler.get()};
        ID3D11Buffer* buffers[] = {v->constants.get()};
        v->context->OMSetRenderTargets(1, views, nullptr);
        v->context->RSSetViewports(1, &viewport);
        v->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        v->context->IASetInputLayout(nullptr); // the triangle comes from SV_VertexID
        v->context->VSSetShader(v->vertex_shader.get(), nullptr, 0);
        v->context->PSSetShader(pixel, nullptr, 0);
        v->context->PSSetShaderResources(0, 7, sources);
        v->context->PSSetSamplers(0, 1, samplers);
        v->context->PSSetConstantBuffers(0, 1, buffers);
        v->context->Draw(3, 0);
    };

    ID3D11RenderTargetView* const final_target =
        second_pass ? v->graded_view.get() : v->target_view.get();
    // Seven slots, of which a frame uses one to five. Binding all seven each
    // time keeps a stale view from a previous frame's shape out of the one
    // being drawn -- and an unused slot is a null the current shader does not
    // declare, rather than a texture it might read.
    // **The chroma's horizontal half first**, for a frame that has chroma:
    // the luma's width at the chroma's height, from the planes, so that the
    // main pass takes its taps downwards from that rather than a square of
    // them from the plane.
    if (ycbcr && v->source_layout.chroma != MP_CHROMA_MONO) {
        const std::uint32_t chroma_height =
            mp_pixel_chroma_height(&v->source_layout, v->source_height);
        if (!make_chroma_wide(v, v->source_width, chroma_height != 0 ? chroma_height : 1u,
                              v->trouble)) {
            return MP_ERR_UNSUPPORTED;
        }
        constants.chroma_planar = mp_pixel_planes(&v->source_layout) == 3u ? 1u : 0u;
        upload_constants();
        ID3D11ShaderResourceView* const from_planes[7] = {
            v->source_view.get(),   v->luma_view.get(),     v->chroma_view.get(),
            v->chroma_u_view.get(), v->chroma_v_view.get(), nullptr,
            v->chroma_weights_view.get()};
        draw(v->pixel_chroma_across.get(), v->chroma_wide_view.get(), v->source_width,
             v->chroma_wide_height, from_planes);
    }
    ID3D11ShaderResourceView* const planes[7] = {
        v->source_view.get(),   v->luma_view.get(),     v->chroma_view.get(),
        v->chroma_u_view.get(), v->chroma_v_view.get(), v->chroma_wide_srv.get(),
        v->chroma_weights_view.get()};

    if (fits && !chained) {
        upload_constants();
        draw(shader, final_target, v->width, v->height, planes);
    } else {
        if (!make_linear_picture(v, v->trouble)) {
            return MP_ERR_UNSUPPORTED;
        }
        // A frame of another size than the chain was told: told again, so
        // that a scaler in it answers for this picture and not the last.
        if (chained &&
            (v->chain_in_width != v->source_width || v->chain_in_height != v->source_height) &&
            !configure_chain(v)) {
            return MP_ERR_UNSUPPORTED;
        }
        constants.emit_linear = 1;
        upload_constants();
        draw(shader, v->linear_picture_view.get(), v->source_width, v->source_height, planes);

        // What the second half reads: the picture, or what the chain made of it.
        ID3D11ShaderResourceView* light = v->linear_picture_srv.get();
        if (chained) {
            // **§9.8.3's chain, between the two halves.** The picture is
            // linear light on the source's own primaries now, at the source's
            // size, which is what a grade is defined on and the only place a
            // lookup table means what its author meant. Unbound first: the
            // intermediate is about to be sampled by somebody else's program.
            ID3D11RenderTargetView* none[] = {nullptr};
            v->context->OMSetRenderTargets(1, none, nullptr);

            MpVideoFrame linear{};
            linear.size = sizeof(linear);
            linear.width = v->source_width;
            linear.height = v->source_height;
            linear.layout = MP_LAYOUT_RGBA32F;
            linear.pts = frame->pts;
            linear.texture = v->linear_picture.get();

            MpVideoFrame graded{};
            if (!run_chain(v, linear, graded) || graded.texture == nullptr) {
                graded = linear;
            }
            // Named, so that what is about to be viewed is a pointer the reader
            // -- and the analyser -- can see cannot be null.
            auto* produced = static_cast<ID3D11Resource*>(graded.texture);
            if (produced == nullptr) {
                v->trouble = "the chain produced no texture and neither did the pass before it";
                return MP_ERR_INTERNAL;
            }
            if (v->chain_out_texture != graded.texture) {
                v->chain_out_view.reset();
                if (FAILED(v->device->CreateShaderResourceView(produced, nullptr,
                                                               v->chain_out_view.put()))) {
                    v->trouble = "what the chain produced would take no view";
                    return MP_ERR_UNSUPPORTED;
                }
                v->chain_out_texture = graded.texture;
            }
            light = v->chain_out_view.get();
        }

        // **The second half, starting from linear.** Tone mapping, the gamut
        // and the output encoding, exactly as they would have run in one pass
        // -- `linear_in` is the only thing that differs, and it is there
        // because decoding a signal that has no transfer would apply one twice.
        // Drawn over the whole target from the whole of what the chain
        // produced: the same size is a fetch at every pixel's own texel, and
        // another size is the sampler's bilinear.
        constants.emit_linear = 0;
        constants.linear_in = 1;
        upload_constants();
        ID3D11ShaderResourceView* const from_light[7] = {light,   nullptr, nullptr, nullptr,
                                                         nullptr, nullptr, nullptr};
        draw(v->pixel_rgba.get(), final_target, v->width, v->height, from_light);
    }

    if (second_pass) {
        // **Unbound first.** The intermediate is about to be read by the video
        // processor or by Direct2D, and a texture that is still a render target
        // is a texture neither of them may sample.
        ID3D11RenderTargetView* none[] = {nullptr};
        v->context->OMSetRenderTargets(1, none, nullptr);

        std::string trouble;
        const bool rolled_off = v->plan.tone_map == mp::video::ToneMap::driver
                                    ? tone_map_by_driver(v, trouble)
                                    : tone_map_by_d2d(v, trouble);
        if (!rolled_off) {
            // **Said once, and then ours instead.** A provider that is not
            // there is not a reason to present an unmapped picture -- §9.1 is
            // that composition clips silently and everything above one is gone
            // -- and it is not a reason to say so on every frame either.
            if (!v->tone_map_complained) {
                v->tone_map_complained = true;
                v->trouble = mp::video::name_of(v->plan.tone_map) + std::string{": "} +
                             trouble + "; using ours instead";
                log_line(MP_LOG_WARN, ("video_d3d11: " + v->trouble).c_str());
            }
            v->plan.tone_map = mp::video::ToneMap::shader;

            // **And this frame is drawn again**, into the real target and with
            // the roll-off in the shader. The first pass wrote HDR10 into an
            // intermediate nobody is now going to read; presenting the target
            // as it stands would be presenting whatever was in it, and dropping
            // the frame would be a gap at exactly the moment a fallback is
            // supposed to be invisible. From here on `maps_elsewhere` is false
            // and there is one pass again.
            constants.emit_pq = 0;
            constants.tone_peak = static_cast<float>(v->plan.tone_target_nits);
            D3D11_MAPPED_SUBRESOURCE again{};
            if (SUCCEEDED(v->context->Map(v->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                          &again))) {
                std::memcpy(again.pData, &constants, sizeof(constants));
                v->context->Unmap(v->constants.get(), 0);
            }
            ID3D11RenderTargetView* real[] = {v->target_view.get()};
            v->context->OMSetRenderTargets(1, real, nullptr);
            v->context->Draw(3, 0);
        }
    }

    if (v->swap_chain) {
        // Not vsynced: §8 makes the audio device the master clock and video
        // drops or duplicates against it, so waiting on the display's own
        // interval would be a second clock arguing with the first.
        v->swap_chain->Present(0, 0);
    }

    ++v->frames;
    v->drawn = true;
    v->last_pts = frame->pts;
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL video_get_device(MpVideo* v, MpGraphicsDevice* out) noexcept
{
    if (v == nullptr || out == nullptr || out->size < sizeof(MpGraphicsDevice::size)) {
        return MP_ERR_INVALID;
    }
    if (!v->device) {
        // Made by `configure`, so before that there is nothing to share -- and
        // saying so is what stops a decoder opening on a null device and
        // quietly falling back to system memory.
        return MP_ERR_UNSUPPORTED;
    }

    MpGraphicsDevice device{};
    device.size = out->size;
    device.api = MP_GRAPHICS_D3D11;
    device.device = v->device.get();
    device.queue = nullptr; // D3D11 has no queue object; D3D12 fills this in
    std::memcpy(out, &device, std::min<std::size_t>(device.size, sizeof(device)));
    return MP_OK;
}

MpResult MP_CALL video_read_back(MpVideo* v, void* dst, std::size_t dst_bytes,
                                 std::uint32_t* out_width, std::uint32_t* out_height,
                                 MpPixelLayout* out_layout) noexcept
try {
    if (v == nullptr || out_width == nullptr || out_height == nullptr ||
        out_layout == nullptr || out_layout->size < sizeof(MpPixelLayout)) {
        return MP_ERR_INVALID;
    }
    *out_width = v->width;
    *out_height = v->height;
    const std::uint32_t caller_size = out_layout->size;
    *out_layout = layout_of(target_format_of(v));
    out_layout->size = caller_size;

    const std::size_t pixel_bytes = mp_pixel_bytes(out_layout);
    const std::size_t needed =
        static_cast<std::size_t>(v->width) * v->height * pixel_bytes;
    if (dst == nullptr || dst_bytes < needed) {
        return MP_TOO_SMALL; // the caller asks again with room, as read_packet does
    }
    if (!v->configured || !v->staging || !v->drawn) {
        return MP_ERR_INVALID;
    }

    v->context->CopyResource(v->staging.get(), v->target.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(v->context->Map(v->staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        v->trouble = "the rendered frame would not map for reading";
        return MP_ERR_INTERNAL;
    }

    // **The pixels as they were rendered, and nothing else.** A row copy,
    // because a staging texture has a pitch of its own and a caller wants the
    // rows to follow one another -- but not a conversion. The first version of
    // this encoded to 8-bit sRGB, which quantised away the dark end where the
    // difference between the sRGB curve and a 2.4 gamma actually lives, and on
    // the HDR10 path read PQ code values as though they were linear.
    const std::size_t row_bytes = static_cast<std::size_t>(v->width) * pixel_bytes;
    auto* out = static_cast<std::uint8_t*>(dst);
    for (std::uint32_t y = 0; y < v->height; ++y) {
        std::memcpy(out + static_cast<std::size_t>(y) * row_bytes,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    row_bytes);
    }
    v->context->Unmap(v->staging.get(), 0);
    return MP_OK;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL video_set(MpVideo* v, const char* key, const char* value) noexcept
try {
    if (v == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "tonemap") == 0) {
        mp::video::ToneMap chosen{};
        if (!mp::video::tone_map_from_name(value, chosen)) {
            return MP_ERR_INVALID;
        }
        v->preferred = chosen;
        return MP_OK;
    }
    if (std::strcmp(key, "device") == 0) {
        // Asked for before `configure`, because that is where the device is
        // made. A test asks for WARP so the pixels are the same every time.
        //
        // **A string rather than a flag, on purpose.** This module knows two
        // values; a presenter on a dedicated video output knows `decklink:0`
        // and its neighbours, and takes them through this same key rather than
        // through an entry point that would have to be added to every module
        // that does not need it.
        if (std::strcmp(value, "warp") == 0) {
            v->warp = true;
        } else if (std::strcmp(value, "hardware") == 0) {
            v->warp = false;
        } else {
            v->trouble = std::string{"this presenter has no device called "} + value +
                         "; it knows hardware and warp";
            return MP_ERR_INVALID;
        }
        return v->device ? MP_ERR_UNSUPPORTED : MP_OK;
    }
    if (std::strcmp(key, "surface") == 0) {
        // **§9.7.1's decision, as a setting.** `window` is what the probe wants
        // -- one program looking at one file -- and `composition` is what an
        // engine wants: a surface handle a shell in another process
        // composites, so a headless engine stays headless and a shell that is
        // killed takes nothing with it. Asked before `configure`, because it
        // decides what kind of chain there is.
        if (v->device) {
            v->trouble = "the surface is decided before the first configure";
            return MP_ERR_BUSY;
        }
        if (std::strcmp(value, "composition") == 0) {
            if (v->window != nullptr) {
                v->trouble = "this presenter was opened on a window, and the "
                             "composition surface is for one that has none";
                return MP_ERR_INVALID;
            }
            std::string why;
            if (!make_composition_handle(v, why)) {
                v->trouble = why;
                return MP_ERR_UNSUPPORTED;
            }
            return MP_OK;
        }
        if (std::strcmp(value, "window") == 0) {
            v->composition = nullptr;
            return MP_OK;
        }
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "display") == 0) {
        // `hdr=1,white=480,peak=1000`, in any order and with any subset given;
        // `probe` goes back to asking the operating system. The names are the
        // fields of §9.4's `Display` and the unit is nits, because that is what
        // every §9 decision is written in.
        if (std::strcmp(value, "probe") == 0) {
            v->display_told = false;
        } else {
            mp::video::Display said{};
            const char* at = value;
            while (*at != '\0') {
                char name[16];
                std::size_t n = 0;
                while (*at != '\0' && *at != '=' && n + 1 < sizeof name) {
                    name[n++] = *at++;
                }
                name[n] = '\0';
                if (*at != '=') {
                    v->trouble = "a display is hdr=0|1, wide=0|1, white=<nits>, "
                                 "peak=<nits>, or the word probe";
                    return MP_ERR_INVALID;
                }
                ++at;
                char* end = nullptr;
                const double number = std::strtod(at, &end);
                if (end == at) {
                    v->trouble = std::string{"`"} + name + "` needs a number";
                    return MP_ERR_INVALID;
                }
                at = end;
                if (std::strcmp(name, "hdr") == 0) {
                    said.hdr = number != 0.0;
                } else if (std::strcmp(name, "wide") == 0) {
                    said.wide = number != 0.0;
                } else if (std::strcmp(name, "white") == 0) {
                    said.sdr_white_nits = number;
                } else if (std::strcmp(name, "peak") == 0) {
                    said.peak_nits = number;
                } else {
                    v->trouble = std::string{"a display has no `"} + name +
                                 "`; it has hdr, wide, white and peak";
                    return MP_ERR_INVALID;
                }
                if (*at == ',') {
                    ++at;
                }
            }
            v->display_told = true;
            v->told_display = said;
        }
        if (!v->configured) {
            return MP_OK; // `configure` will read it
        }
        return replan(v) ? MP_OK : MP_ERR_UNSUPPORTED;
    }
    if (std::strcmp(key, "chroma") == 0) {
        // **The kernel, by name** (kernels.hpp), for the chroma
        // reconstruction, settable between frames because it is read per
        // frame. The picture's own resampling is a stage's (§9.11), and its
        // keys are the stage's.
        mp::kernels::Kernel kernel{};
        if (!mp::kernels::kernel_from_name(value, kernel)) {
            v->trouble = mp::kernels::k_kernel_sentence;
            return MP_ERR_INVALID;
        }
        v->chroma_kernel = kernel;
        return MP_OK;
    }
    if (std::strcmp(key, "chroma_lobes") == 0) {
        // One is the floor because a kernel with no lobes has no support;
        // there is no ceiling, for the reason the stage gives.
        char* end = nullptr;
        const unsigned long lobes = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || lobes == 0 || lobes > 0xfffffffful) {
            v->trouble = "chroma_lobes is a whole number from 1 up";
            return MP_ERR_INVALID;
        }
        v->chroma_lobes = static_cast<std::uint32_t>(lobes);
        return MP_OK;
    }
    if (std::strcmp(key, "chroma_b") == 0 || std::strcmp(key, "chroma_c") == 0) {
        char* end = nullptr;
        const double number = std::strtod(value, &end);
        if (end == value || *end != '\0') {
            v->trouble = std::string{key} + " is a number";
            return MP_ERR_INVALID;
        }
        (key[7] == 'b' ? v->chroma_b : v->chroma_c) = number;
        return MP_OK;
    }
    if (std::strcmp(key, "siting") == 0) {
        // Where a chroma sample sits: `auto` is the stream's own statement,
        // and type 0 where it made none.
        if (std::strcmp(value, "auto") == 0) {
            v->siting_asked = MpVideo::k_siting_auto;
            return MP_OK;
        }
        std::uint32_t type = 0;
        if (!mp::video::siting_from_name(value, type)) {
            v->trouble = "a siting is auto, left, centre, topleft, top, bottomleft or bottom";
            return MP_ERR_INVALID;
        }
        v->siting_asked = type;
        return MP_OK;
    }
    if (std::strcmp(key, "gamut") == 0) {
        mp::video::Gamut gamut{};
        if (!mp::video::gamut_from_name(value, gamut)) {
            v->trouble = "a gamut is clip or desaturate";
            return MP_ERR_INVALID;
        }
        v->gamut = gamut;
        return MP_OK;
    }
    if (std::strcmp(key, "size") == 0) {
        // **§9.7.1's other decision, as a setting.** `WxH` in pixels, or
        // `native` for the picture's own size. Settable at any time, before
        // `configure` and between frames, because a window that has been
        // resized is exactly this key arriving again.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (std::strcmp(value, "native") != 0) {
            char* end = nullptr;
            const unsigned long asked_width = std::strtoul(value, &end, 10);
            if (end == value || (*end != 'x' && *end != 'X')) {
                v->trouble = "a size is WxH, in pixels, or native";
                return MP_ERR_INVALID;
            }
            const char* rest = end + 1;
            const unsigned long asked_height = std::strtoul(rest, &end, 10);
            // **`k_max_dimension` is Direct3D's number, and this is Direct3D's
            // module.** `D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION` is 16384 at
            // feature level 11, so a size past it is one this presenter cannot
            // ever satisfy -- not a taste, and not a guess about somebody
            // else's hardware. Refusing it by name here is better than a
            // `CreateTexture2D` that fails with an HRESULT and no number in
            // it.
            //
            // What would be wrong is putting it anywhere above this file. §3
            // keeps the core and the player portable, `mp::VideoPath` forwards
            // a size without an opinion about it, and a Metal or a Vulkan
            // presenter has its own limit and a different one. A cross-platform
            // layer that knew 16384 would be a layer that had learned DXGI.
            if (end == rest || *end != '\0' || asked_width == 0 || asked_height == 0 ||
                asked_width > k_max_dimension || asked_height > k_max_dimension) {
                v->trouble = "a size is WxH, in pixels, neither zero nor over 16384, "
                             "which is what Direct3D 11 will make a texture of";
                return MP_ERR_INVALID;
            }
            width = static_cast<std::uint32_t>(asked_width);
            height = static_cast<std::uint32_t>(asked_height);
        }
        if (width == v->asked_width && height == v->asked_height) {
            return MP_OK;
        }
        v->asked_width = width;
        v->asked_height = height;
        if (!v->configured) {
            return MP_OK; // `configure` will read it
        }
        return resize_target(v) ? MP_OK : MP_ERR_UNSUPPORTED;
    }
    if (std::strcmp(key, "precision") == 0) {
        // Off-screen only, and it is the difference between measuring the
        // pipeline and measuring what a display gets. Both are worth doing:
        // `fp32` is the arithmetic, `fp16` is the arithmetic plus the rounding
        // DXGI imposes on everybody.
        if (std::strcmp(value, "fp32") == 0) {
            v->wide_target = true;
        } else if (std::strcmp(value, "fp16") == 0) {
            v->wide_target = false;
        } else {
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "composited") == 0) {
        v->composited = std::strcmp(value, "0") != 0;
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL video_stages(MpVideo* v, const MpVideoStage* stages,
                              std::uint32_t count) noexcept
try {
    if (v == nullptr || (count != 0 && stages == nullptr)) {
        return MP_ERR_INVALID;
    }

    std::vector<MpVideoStage> taken;
    taken.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        // Read no further than the caller says its struct goes, which is what
        // the size prefix is for.
        MpVideoStage one{};
        std::memcpy(&one, &stages[i],
                    std::min<std::size_t>(stages[i].size, sizeof(one)));
        if (one.vtbl == nullptr || one.handle == nullptr ||
            one.vtbl->size < sizeof(MpVideoDspVtbl)) {
            v->trouble = "a stage with no vtable, or one older than this presenter reads";
            return MP_ERR_INVALID;
        }
        taken.push_back(one);
    }
    v->chain = std::move(taken);

    if (!v->configured) {
        return MP_OK; // `configure` will make what the chain needs
    }
    // The chain is told what it will be given; the intermediate it reads is
    // made when a frame first needs it. The target, the swap chain and the
    // colour plan are all about the display, and a chain does not move the
    // display.
    return configure_chain(v) ? MP_OK : MP_ERR_UNSUPPORTED;
}
MEDIAPERCH_ABI_GUARD_CATCH

MpResult MP_CALL video_describe(MpVideo* v, std::uint32_t index, char* out,
                                std::uint32_t out_bytes, std::uint32_t* out_needed) noexcept
try {
    if (v == nullptr || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    mp::DescribeRow row{out, out_bytes, out_needed};
    switch (index) {
    case 0:
        row.format("tonemap\t%s\thow HDR reaches an SDR display: none, driver, d2d, shader"
                   "\tenum:none,driver,d2d,shader group=Colour",
                   mp::video::name_of(v->preferred));
        return row.result();
    case 1:
        row.format("device\t%s\thardware or warp; warp is deterministic"
                   "\tenum:hardware,warp group=Device",
                   v->warp ? "warp" : "hardware");
        return row.result();
    case 2:
        row.format("composited\t%d\twhether anything is drawn over the video\tbool group=Device",
                   v->composited ? 1 : 0);
        return row.result();
    case 3:
        // **The handle is a number on purpose.** It has to cross a process
        // boundary, and a shell duplicates it by value out of an IPC message;
        // printing it is how a settings surface carries one.
        if (v->composition != nullptr) {
            const HANDLE waitable = v->waitable;
            // A handle is a number Windows gives the type void*, 32 bits wide
            // so that it can cross into 32-bit code; HandleToULong is the
            // SDK's word for that number.
            row.format("surface\tcomposition 0x%lx, waitable 0x%lx"
                       "\twhere it draws and what paces it (read only)",
                       HandleToULong(v->composition), HandleToULong(waitable));
        } else {
            row.format("surface\t%s\twhere it draws (read only)",
                       v->window != nullptr ? "a window" : "off-screen");
        }
        return row.result();
    case 4:
        // **Three numbers rather than one word.** §9.6's boost is a function of
        // the first, §9.9.1's HLG OOTF of the second, and until both were
        // printed the only way to tell a display that was asked from one that
        // was assumed was to read the source.
        row.format("display\t%s, white %.0f nits, peak %.0f nits"
                   "\twhat it turned out to be (read only)",
                   v->display.hdr ? "HDR" : "SDR",
                   v->display.sdr_white_nits, v->display.peak_nits);
        return row.result();
    case 5:
        row.format("encoding\t%s\twhat the buffer holds (read only)",
                   mp::video::name_of(v->plan.encoding));
        return row.result();
    case 6:
        row.format("applied\t%s\tthe tone mapper in the path (read only)",
                   v->plan.tone_mapping ? mp::video::name_of(v->plan.tone_map) : "none");
        return row.result();
    case 7:
        row.format("sdr_scale\t%.4f\twhat SDR content is multiplied by (read only)",
                   v->plan.sdr_scale);
        return row.result();
    case 8:
        row.format("frames\t%llu\tpresented so far (read only)",
                   static_cast<unsigned long long>(v->frames));
        return row.result();
    case 9:
        row.format("precision\t%s\twhat it renders into; fp16 is what a display gets "
                   "and all DXGI will present\tenum:fp16,fp32 group=Device",
                   off_screen(v) && v->wide_target ? "fp32" : "fp16");
        return row.result();
    case 10:
        row.format("convert\t%s\twhat the source transfer needs (read only)",
                   mp::video::name_of(v->plan.convert));
        return row.result();
    case 11:
        row.format("trouble\t%s\twhat went wrong (read only)",
                   v->trouble.empty() ? "nothing" : v->trouble.c_str());
        return row.result();
    case 12:
        // **What was asked for, not what it came to.** This row round-trips
        // through `set`, so `native` has to read back as `native` rather than
        // as the size it happens to resolve to; `picture` below is where the
        // number comes from.
        if (v->asked_width != 0) {
            row.format("size\t%ux%u\twhat it renders at; a shell sets this when its "
                       "window changes\tsize group=Picture",
                       v->asked_width, v->asked_height);
        } else {
            row.format("size\tnative\twhat it renders at; a shell sets this when its "
                       "window changes\tsize group=Picture");
        }
        return row.result();
    case 13:
        // **The number a shell cannot work out for itself.** It has the
        // window; it does not have the container's aspect correction, and a
        // shell that guessed at it would stretch every anamorphic file.
        row.format("picture\t%ux%u\tthe video's own size, which is what a shell fits "
                   "(read only)",
                   v->picture_width, v->picture_height);
        return row.result();
    case 15:
        // **§9.8.3's chain, and whether it is actually in the path.** A stage
        // that was handed over and an intermediate that was made are two
        // things, and a run that says one without the other is a run drawing
        // one pass while somebody believes it is grading.
        if (v->chain.empty()) {
            row.format("chain\tnone\twhat runs in linear light: nothing, so the picture "
                       "is one pass unless the target is another size (read only)");
        } else if (v->linear_picture) {
            row.format("chain\t%zu stage%s, with an intermediate at %ux%u\twhat runs in "
                       "linear light, and the picture it is given (read only)",
                       v->chain.size(), v->chain.size() == 1 ? "" : "s",
                       v->linear_width, v->linear_height);
        } else {
            row.format("chain\t%zu stage%s, no frame yet\twhat runs in linear light "
                       "(read only)",
                       v->chain.size(), v->chain.size() == 1 ? "" : "s");
        }
        return row.result();
    case 14:
        // **Who said what the display is**, which is the difference between a
        // decision and a guess. A windowless engine that fell back to the first
        // output would report the same three numbers and be wrong about which
        // monitor they belong to, and nothing else in the report would say so.
        row.format("display_from\t%s\twho said what the display is (read only)",
                   v->display_told ? "the shell" : "this process");
        return row.result();
    case 16:
        // **The two numbers the roll-off is made of**, so a person can see
        // that 203 is what an SDR display's white means to HDR content and
        // 1000 is what the file was graded to -- or what it was taken to be.
        if (v->plan.tone_mapping && v->plan.tone_map == mp::video::ToneMap::shader) {
            row.format("target\t%.0f nits from %.0f\twhere the roll-off aims (BT.2408's "
                       "reference white on an SDR display) and the source peak it "
                       "starts from (read only)",
                       v->plan.tone_target_nits, v->plan.tone_source_nits);
        } else {
            row.format("target\tnone\twhere the roll-off aims; nothing is rolled off "
                       "(read only)");
        }
        return row.result();
    case 17:
        row.format("chroma\t%s\tthe kernel chroma is reconstructed with at the luma's "
                   "positions\tenum:%s group=Chroma",
                   mp::kernels::name_of(v->chroma_kernel), mp::kernels::k_kernel_list);
        return row.result();
    case 18:
        row.format("chroma_lobes\t%u\tLanczos lobes for the chroma reconstruction\tint "
                   "min=1 step=1 group=Chroma when=chroma=lanczos",
                   v->chroma_lobes);
        return row.result();
    case 19:
        row.format("chroma_b\t%.4f\tthe cubic's B, when chroma=bicubic\tnumber step=0.05 "
                   "group=Chroma when=chroma=bicubic",
                   v->chroma_b);
        return row.result();
    case 20:
        row.format("chroma_c\t%.4f\tthe cubic's C, when chroma=bicubic\tnumber step=0.05 "
                   "group=Chroma when=chroma=bicubic",
                   v->chroma_c);
        return row.result();
    case 21:
        // **Which siting, and whose.** A person's, the stream's, or the
        // assumption -- and the row says which, because a quarter-sample
        // shift is invisible in a colour and plain in a test pattern.
        if (v->siting_asked < mp::video::k_siting_types) {
            row.format("siting\t%s\twhere a chroma sample sits against the luma: auto, "
                       "left, centre, topleft, top, bottomleft, bottom"
                       "\tenum:auto,left,centre,topleft,top,bottomleft,bottom group=Chroma",
                       mp::video::siting_name(v->siting_asked));
        } else {
            row.format("siting\tauto (%s, %s)\twhere a chroma sample sits against the "
                       "luma: auto, left, centre, topleft, top, bottomleft, bottom"
                       "\tenum:auto,left,centre,topleft,top,bottomleft,bottom group=Chroma",
                       mp::video::siting_name(v->siting),
                       v->stream_siting != 0 ? "the stream's" : "assumed");
        }
        return row.result();
    case 22:
        row.format("gamut\t%s\twhat a colour past the display's gamut gets: clip, "
                   "desaturate\tenum:clip,desaturate group=Colour",
                   mp::video::name_of(v->gamut));
        return row.result();
    case 23:
        // **Source to target, the factor, and who scales**: a stage in the
        // chain that answered the target's size, or the finish pass's own
        // bilinear fetch, which is what a chain with no scaler leaves.
        if (v->source_width == 0) {
            row.format("scale\tno frame yet\tsource to target, the factor, and who "
                       "scales (read only)");
        } else if (!resampling(v)) {
            row.format("scale\t%ux%u 1:1\tsource to target: one to one is every sample "
                       "read where it is (read only)",
                       v->source_width, v->source_height);
        } else if (v->chain_out_width == v->width && v->chain_out_height == v->height) {
            row.format("scale\t%ux%u -> %ux%u, x%.3f by the chain\tsource to target and "
                       "the factor; a stage in the chain produces the target's size "
                       "(read only)",
                       v->source_width, v->source_height, v->width, v->height,
                       static_cast<double>(v->width) / static_cast<double>(v->source_width));
        } else {
            row.format("scale\t%ux%u -> %ux%u, x%.3f by the presenter's bilinear "
                       "fetch\tsource to target and the factor; no stage in the chain "
                       "scales, so the finish pass samples the picture with two taps -- "
                       "put `scale` in video_dsp for a kernel (read only)",
                       v->source_width, v->source_height, v->width, v->height,
                       static_cast<double>(v->width) / static_cast<double>(v->source_width));
        }
        return row.result();
    default:
        break;
    }
    return MP_END;
}
MEDIAPERCH_ABI_GUARD_CATCH

const MpVideoVtbl g_vtbl = {
    /* size      */ sizeof(MpVideoVtbl),
    /* reserved  */ 0,
    /* open      */ &video_open,
    /* close     */ &video_close,
    /* configure */ &video_configure,
    /* present   */ &video_present,
    /* set        */ &video_set,
    /* describe   */ &video_describe,
    /* get_device */ &video_get_device,
    /* read_back  */ &video_read_back,
    /* stages     */ &video_stages,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VIDEO,
    /* priority    */ 100,
    /* id          */ "video_d3d11",
    /* name        */ "Direct3D 11 (flip model, scRGB, and a frame you can hash)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ nullptr,
    /* codec_count */ 0,
    /* reserved    */ 0,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

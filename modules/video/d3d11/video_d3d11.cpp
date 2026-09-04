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
// What is not here yet, and why. NV12 and P010 arrive now -- as planes and,
// from a hardware decoder, as a texture this module views in place -- so the
// half of this paragraph that said otherwise is gone with the decoder that
// made it true. What is left is the colour work §9 turns on: the transfer
// functions here are sRGB and BT.1886, so a PQ or an HLG source is decoded
// with the wrong curve rather than refused, and the tone mappers are chosen
// and reported and not applied. §9.9.1 says HLG goes on the linear path in
// every case, which is the shape that work takes.

#include "colour_plan.hpp"
#include "yuv_matrix.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
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

private:
    T* p_ = nullptr;
};

// --------------------------------------------------------------------------
// The shader
// --------------------------------------------------------------------------
//
// **One triangle, not two.** A full-screen triangle covering the viewport
// wastes a quarter of its area and rasterises with no diagonal seam, where a
// quad has one down the middle that shows up in exactly the gradients this is
// for. The vertex shader makes it from `SV_VertexID` and there is no vertex
// buffer at all.
//
// The pixel shader is §9.6 and nothing else yet: sRGB in, linear out, scaled by
// what the display says its white is. Writing it as a shader rather than as a
// lookup is what lets the tone mappers join it later without another pass.
constexpr char k_shader[] = R"HLSL(
Texture2D<float4> rgba   : register(t0);   // the BGRA8 path
Texture2D<float>  luma   : register(t1);   // NV12 / P010 Y
Texture2D<float2> chroma : register(t2);   // NV12 / P010 CbCr
SamplerState bilinear : register(s0);

cbuffer Constants : register(b0)
{
    float sdr_scale;      // §9.6: the display's white over scRGB's 80 nits
    float luma_offset;    // studio range puts black at 16/255
    float luma_scale;
    float chroma_scale;

    float4 yuv;           // r_v, g_u, g_v, b_u -- see yuv_matrix.hpp

    float sample_scale;   // P010's ten bits sit in the top of sixteen
    float transfer_gamma; // 2.4 for BT.1886, unused when srgb_piecewise is 1
    float srgb_piecewise; // 1 = the sRGB curve, 0 = a pure power
    float padding;
};

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID)
{
    // (0,0) (2,0) (0,2) in UV, which is one triangle over the whole viewport.
    Vertex out_vertex;
    out_vertex.uv = float2((id << 1) & 2, id & 2);
    out_vertex.position = float4(out_vertex.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return out_vertex;
}

// **The transfer decode, by the code point the container stated.**
//
// Two curves, and which one is not a matter of taste. Content tagged sRGB gets
// the sRGB piecewise curve. Content tagged BT.709 or BT.601 gets BT.1886, a
// pure 2.4 power, because that is the reference display EOTF those standards
// specify and it is what the picture was graded on -- decoding video with the
// sRGB curve instead lifts the shadows, which is the mirror image of the fault
// §9.2 records Windows committing in the other direction.
float3 to_linear(float3 c)
{
    // Clamped, then abs: a UNORM texture cannot be negative, but the compiler
    // cannot prove it and pow of a negative base is undefined -- which is what
    // X3571 says, and this file is built with warnings as errors precisely so
    // that a shader nobody reads cannot quietly contain one.
    c = saturate(c);
    float3 piecewise = c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
    float3 power = pow(abs(c), transfer_gamma);
    return lerp(power, piecewise, srgb_piecewise);
}

float4 ps_rgba(Vertex input) : SV_Target
{
    float4 texel = rgba.Sample(bilinear, input.uv);
    // **The target is scRGB, which is linear.** An sRGB texture sampled without
    // decoding would be presented as though its gamma were already gone, which
    // is the washed-out picture people mistake for a tone mapping problem.
    return float4(to_linear(texel.rgb) * sdr_scale, texel.a);
}

float4 ps_nv12(Vertex input) : SV_Target
{
    // Chroma is sampled bilinearly at half resolution, which is the
    // reconstruction 4:2:0 asks for and is what every player does. Nothing here
    // pretends it is the chroma siting a stream may have stated: that is a
    // quarter-pixel shift and it belongs with the tone mappers.
    float  y  = luma.Sample(bilinear, input.uv).r * sample_scale;
    float2 uv = chroma.Sample(bilinear, input.uv).rg * sample_scale;

    y = (y - luma_offset) * luma_scale;
    float u = (uv.x - 0.5) * chroma_scale;
    float v = (uv.y - 0.5) * chroma_scale;

    // Non-linear R'G'B' first, which is what the matrix produces, and then the
    // transfer. Doing them the other way round is a mistake that looks almost
    // right and is wrong everywhere the picture is not grey.
    float3 encoded = float3(y + yuv.x * v,
                            y - yuv.y * u - yuv.z * v,
                            y + yuv.w * u);
    return float4(to_linear(encoded) * sdr_scale, 1.0);
}
)HLSL";

/// Mirrors the `cbuffer` above, which HLSL packs in four-float rows.
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
    float srgb_piecewise = 1.0f;
    float padding = 0.0f;
};

/// What §9.4 could work out, given a device and a window. Kept separate from
/// the plan so the plan stays testable.
mp::video::Display probe_display(IDXGIFactory2* factory, HWND window)
{
    mp::video::Display display{};
    if (factory == nullptr) {
        return display;
    }

    // **The output with the greatest intersection with the window**, which §9.4
    // says to use rather than `IDXGISwapChain::GetContainingOutput` -- that one
    // returns a stale output, and the obvious fix of recreating the swap chain
    // flashes black. With no window there is nothing to intersect, so the first
    // output of the first adapter stands in.
    Com<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, adapter.put()))) {
        return display;
    }
    Com<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, output.put()))) {
        return display;
    }
    Com<IDXGIOutput6> output6;
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput6),
                                      reinterpret_cast<void**>(output6.put())))) {
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

    (void)window;
    // The SDR white level comes from QueryDisplayConfig, which needs the
    // window's monitor to pick a path. Without one the scRGB reference stands,
    // and 80 nits is a scale of exactly one rather than a guess.
    return display;
}

} // namespace

struct MpVideo {
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

    Com<ID3D11VertexShader> vertex_shader;
    /// One per source kind rather than a branch in one shader: a constant that
    /// is the same for every pixel of every frame is not a thing to test at
    /// every pixel of every frame.
    Com<ID3D11PixelShader> pixel_rgba;
    Com<ID3D11PixelShader> pixel_nv12;
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
    Com<ID3D11Texture2D> chroma;
    Com<ID3D11ShaderResourceView> chroma_view;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    MpPixelFormat source_format = MP_PIXEL_NONE;

    HWND window = nullptr;
    bool warp = false;
    /// Off-screen only: single precision unless a caller asks for the format a
    /// display would actually get. A swap chain has no say -- FP16 is the most
    /// DXGI will present.
    bool wide_target = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    mp::video::Stream stream{};
    /// `MP_VIDEO_FULL_RANGE` from the container. Studio range is the default
    /// and the safe one: treating 16..235 as 0..255 crushes the blacks and
    /// clips the whites, which reads as a contrast setting rather than a bug.
    bool full_range = false;
    mp::video::Display display{};
    mp::video::Plan plan{};
    mp::video::ToneMap preferred = mp::video::ToneMap::driver;
    bool composited = true;
    bool configured = false;
    std::uint64_t frames = 0;
    std::uint64_t last_pts = 0;

    std::string trouble;
};

namespace {

bool compile(const char* entry, const char* target, Com<ID3DBlob>& out, std::string& why)
{
    Com<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(k_shader, sizeof(k_shader) - 1, "colour.hlsl", nullptr,
                                    nullptr, entry, target,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                                    0, out.put(), errors.put());
    if (SUCCEEDED(hr)) {
        return true;
    }
    why = "the colour shader would not compile: ";
    if (errors) {
        why += static_cast<const char*>(errors->GetBufferPointer());
    } else {
        char code[32];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        why += code;
    }
    return false;
}

/// The device, and the two ways of getting one.
///
/// **It is made for two jobs.** §9.8.1 puts the device here rather than in the
/// decoder -- a presenter is made once and outlives every decoder a playlist
/// goes through -- so a decoder that will be handed this one needs two things
/// the presenting half never asks for: `ID3D11VideoDevice`, which comes of
/// `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, and multithread protection, because
/// Media Foundation decodes on threads of its own. Both are free to the
/// presenting half and neither can be added afterwards.
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
                                            reinterpret_cast<void**>(threading.put())))) {
        threading->SetMultithreadProtected(TRUE);
    }

    Com<IDXGIDevice> dxgi;
    if (FAILED(v->device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(dxgi.put())))) {
        why = "the device is not a DXGI device, which cannot happen";
        return false;
    }
    Com<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(v->factory.put())))) {
        why = "no DXGI factory";
        return false;
    }
    return true;
}

bool make_shaders(MpVideo* v, std::string& why)
{
    Com<ID3DBlob> vs;
    Com<ID3DBlob> rgba;
    Com<ID3DBlob> nv12;
    if (!compile("vs_main", "vs_5_0", vs, why) ||
        !compile("ps_rgba", "ps_5_0", rgba, why) ||
        !compile("ps_nv12", "ps_5_0", nv12, why)) {
        return false;
    }
    if (FAILED(v->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                             nullptr, v->vertex_shader.put())) ||
        FAILED(v->device->CreatePixelShader(rgba->GetBufferPointer(), rgba->GetBufferSize(),
                                            nullptr, v->pixel_rgba.put())) ||
        FAILED(v->device->CreatePixelShader(nv12->GetBufferPointer(), nv12->GetBufferSize(),
                                            nullptr, v->pixel_nv12.put()))) {
        why = "the colour shader compiled and would not load";
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
DXGI_FORMAT target_format_of(const MpVideo* v) noexcept
{
    if (v->window == nullptr && v->wide_target &&
        v->plan.encoding == mp::video::Encoding::linear) {
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    return dxgi_format_of(v->plan);
}

MpPixelFormat pixel_format_of(DXGI_FORMAT f) noexcept
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return MP_PIXEL_RGBA32F;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return MP_PIXEL_RGB10A2;
    default:
        return MP_PIXEL_RGBA16F;
    }
}

std::size_t pixel_bytes_of(MpPixelFormat f) noexcept
{
    switch (f) {
    case MP_PIXEL_RGBA32F:
        return 16u;
    case MP_PIXEL_RGBA16F:
        return 8u;
    default:
        return 4u;
    }
}

DXGI_COLOR_SPACE_TYPE colour_space_of(const mp::video::Plan& plan) noexcept
{
    // scRGB is linear with the BT.709 primaries; HDR10 is PQ with BT.2020.
    return dxgi_format_of(plan) == DXGI_FORMAT_R10G10B10A2_UNORM
               ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
               : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

/// The swap chain, or the texture that stands in for one.
bool make_target(MpVideo* v, std::string& why)
{
    v->target_view.reset();
    v->target.reset();
    v->staging.reset();
    v->swap_chain.reset();

    const DXGI_FORMAT format = target_format_of(v);

    if (v->window != nullptr) {
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
                __uuidof(IDXGISwapChain3), reinterpret_cast<void**>(chain3.put())))) {
            const DXGI_COLOR_SPACE_TYPE space = colour_space_of(v->plan);
            UINT support = 0;
            if (SUCCEEDED(chain3->CheckColorSpaceSupport(space, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
                chain3->SetColorSpace1(space);
            }
        }
        if (FAILED(v->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(v->target.put())))) {
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
    staging.Format = format;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(v->device->CreateTexture2D(&staging, nullptr, v->staging.put()))) {
        why = "no staging texture, so nothing could be read back";
        return false;
    }
    return true;
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

    const bool ten_bit = desc.Format == DXGI_FORMAT_P010;
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
    v->source_width = frame.width != 0 ? frame.width : desc.Width;
    v->source_height = frame.height != 0 ? frame.height : desc.Height;
    v->source_format = ten_bit ? MP_PIXEL_P010 : MP_PIXEL_NV12;
    return true;
}

/// The frame, in whichever of the three shapes it arrived in.
bool upload(MpVideo* v, const MpVideoFrame& frame, std::string& why)
{
    const bool planar = frame.format == MP_PIXEL_NV12 || frame.format == MP_PIXEL_P010;
    if (!planar && frame.format != MP_PIXEL_BGRA8) {
        why = "this presenter takes BGRA8, NV12 and P010";
        return false;
    }
    if (frame.texture != nullptr) {
        return adopt(v, frame, why);
    }
    if (frame.width == 0 || frame.height == 0) {
        why = "a frame with no pixels in it";
        return false;
    }

    const bool changed = v->source_width != frame.width ||
                         v->source_height != frame.height ||
                         v->source_format != frame.format;
    if (changed) {
        v->source_view.reset();
        v->source.reset();
        v->luma_view.reset();
        v->luma.reset();
        v->chroma_view.reset();
        v->chroma.reset();
        v->source_width = frame.width;
        v->source_height = frame.height;
        v->source_format = frame.format;
    }

    if (!planar) {
        if (frame.plane[0] == nullptr || frame.stride[0] < frame.width * 4u) {
            why = "the frame has no pixels, or a stride too short for its width";
            return false;
        }
        if (!v->source &&
            !make_plane(v, frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                        v->source, v->source_view, why)) {
            return false;
        }
        return write_plane(v, v->source.get(), frame.plane[0], frame.stride[0],
                           frame.width * 4u, frame.height, why);
    }

    // 4:2:0, so the chroma plane is half in each direction -- rounded up,
    // because an odd width still has a chroma column for its last pixel.
    const std::uint32_t chroma_width = (frame.width + 1u) / 2u;
    const std::uint32_t chroma_height = (frame.height + 1u) / 2u;
    const bool ten_bit = frame.format == MP_PIXEL_P010;
    const std::uint32_t luma_bytes = ten_bit ? 2u : 1u;

    if (frame.plane[0] == nullptr || frame.plane[1] == nullptr ||
        frame.stride[0] < frame.width * luma_bytes ||
        frame.stride[1] < chroma_width * luma_bytes * 2u) {
        why = "a planar frame with a missing plane, or a stride too short for its width";
        return false;
    }

    if (!v->luma &&
        (!make_plane(v, frame.width, frame.height,
                     ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, v->luma,
                     v->luma_view, why) ||
         !make_plane(v, chroma_width, chroma_height,
                     ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM,
                     v->chroma, v->chroma_view, why))) {
        return false;
    }
    return write_plane(v, v->luma.get(), frame.plane[0], frame.stride[0],
                       frame.width * luma_bytes, frame.height, why) &&
           write_plane(v, v->chroma.get(), frame.plane[1], frame.stride[1],
                       chroma_width * luma_bytes * 2u, chroma_height, why);
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
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL video_close(MpVideo* v) noexcept
{
    delete v;
}

MpResult MP_CALL video_configure(MpVideo* v, const MpVideoInfo* in) noexcept
try {
    if (v == nullptr || in == nullptr || in->width == 0 || in->height == 0) {
        return MP_ERR_INVALID;
    }

    v->stream = mp::video::Stream{.primaries = in->primaries,
                                  .transfer = in->transfer,
                                  .matrix = in->matrix,
                                  .width = in->width,
                                  .height = in->height};
    v->full_range = (in->flags & MP_VIDEO_FULL_RANGE) != 0;
    v->width = in->display_width != 0 ? in->display_width : in->width;
    v->height = in->display_height != 0 ? in->display_height : in->height;

    if (!v->device && !make_device(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    if (!v->vertex_shader && !make_shaders(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }

    v->display = probe_display(v->factory.get(), v->window);
    v->plan = mp::video::plan_for(v->stream, v->display, v->preferred, v->composited);

    if (!make_target(v, v->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }
    v->configured = true;
    v->frames = 0;
    v->trouble.clear();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

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

    const bool planar = v->source_format == MP_PIXEL_NV12 ||
                        v->source_format == MP_PIXEL_P010;

    Constants constants{};
    constants.sdr_scale = v->plan.sdr_scale;

    // **The transfer, by the code point the container stated.** sRGB gets the
    // piecewise curve; BT.709 and BT.601 video gets BT.1886, a pure 2.4, which
    // is the reference display EOTF those standards specify and what the
    // picture was graded on. Decoding video with the sRGB curve instead lifts
    // the shadows, which is the mirror image of §9.2's complaint.
    const std::uint32_t transfer = mp::video::assumed_transfer(v->stream);
    constants.srgb_piecewise = transfer == mp::video::k_transfer_srgb ? 1.0f : 0.0f;
    constants.transfer_gamma = 2.4f;

    if (planar) {
        // Derived in double and rounded once, which is §9.10's rule and this
        // is where it pays: eight coefficients per matrix, all of them ratios
        // of the luma weights.
        const mp::video::YuvMatrix m = mp::video::yuv_matrix_for(
            v->stream.matrix, v->full_range, v->source_format == MP_PIXEL_P010);
        constants.luma_offset = m.luma_offset;
        constants.luma_scale = m.luma_scale;
        constants.chroma_scale = m.chroma_scale;
        constants.r_v = m.r_v;
        constants.g_u = m.g_u;
        constants.g_v = m.g_v;
        constants.b_u = m.b_u;
        constants.sample_scale = m.sample_scale;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(v->context->Map(v->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                  &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        v->context->Unmap(v->constants.get(), 0);
    }

    const D3D11_VIEWPORT viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(v->width),
                                  static_cast<float>(v->height),
                                  0.0f,
                                  1.0f};
    ID3D11RenderTargetView* views[] = {v->target_view.get()};
    // Three slots, of which a frame uses one or two. Binding all three each
    // time keeps a stale view from a previous frame's shape out of the one
    // being drawn.
    ID3D11ShaderResourceView* resources[] = {v->source_view.get(), v->luma_view.get(),
                                             v->chroma_view.get()};
    ID3D11SamplerState* samplers[] = {v->sampler.get()};
    ID3D11Buffer* buffers[] = {v->constants.get()};

    v->context->OMSetRenderTargets(1, views, nullptr);
    v->context->RSSetViewports(1, &viewport);
    v->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    v->context->IASetInputLayout(nullptr); // the triangle comes from SV_VertexID
    v->context->VSSetShader(v->vertex_shader.get(), nullptr, 0);
    v->context->PSSetShader(planar ? v->pixel_nv12.get() : v->pixel_rgba.get(), nullptr,
                            0);
    v->context->PSSetShaderResources(0, 3, resources);
    v->context->PSSetSamplers(0, 1, samplers);
    v->context->PSSetConstantBuffers(0, 1, buffers);
    v->context->Draw(3, 0);

    if (v->swap_chain) {
        // Not vsynced: §8 makes the audio device the master clock and video
        // drops or duplicates against it, so waiting on the display's own
        // interval would be a second clock arguing with the first.
        v->swap_chain->Present(0, 0);
    }

    ++v->frames;
    v->last_pts = frame->pts;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

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
                                 MpPixelFormat* out_format) noexcept
try {
    if (v == nullptr || out_width == nullptr || out_height == nullptr ||
        out_format == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_width = v->width;
    *out_height = v->height;
    *out_format = pixel_format_of(target_format_of(v));

    const std::size_t pixel_bytes = pixel_bytes_of(*out_format);
    const std::size_t needed =
        static_cast<std::size_t>(v->width) * v->height * pixel_bytes;
    if (dst == nullptr || dst_bytes < needed) {
        return MP_ERR_NO_MEMORY; // the caller asks again with room, as read_packet does
    }
    if (!v->configured || !v->staging || v->frames == 0) {
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
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

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
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL video_describe(MpVideo* v, std::uint32_t index, char* out,
                                std::uint32_t out_bytes) noexcept
try {
    if (v == nullptr || out == nullptr || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes,
                      "tonemap\t%s\thow HDR reaches an SDR display: none, driver, d2d, shader",
                      mp::video::name_of(v->preferred));
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes, "device\t%s\thardware or warp; warp is deterministic",
                      v->warp ? "warp" : "hardware");
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes,
                      "composited\t%d\twhether anything is drawn over the video",
                      v->composited ? 1 : 0);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes, "surface\t%s\twhere it draws (read only)",
                      v->window != nullptr ? "a window" : "off-screen");
        return MP_OK;
    case 4:
        std::snprintf(out, out_bytes, "display\t%s\twhat it turned out to be (read only)",
                      v->display.hdr ? "HDR" : "SDR");
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes,
                      "encoding\t%s\twhat the buffer holds (read only)",
                      mp::video::name_of(v->plan.encoding));
        return MP_OK;
    case 6:
        std::snprintf(out, out_bytes, "applied\t%s\tthe tone mapper in the path (read only)",
                      v->plan.tone_mapping ? mp::video::name_of(v->plan.tone_map) : "none");
        return MP_OK;
    case 7:
        std::snprintf(out, out_bytes,
                      "sdr_scale\t%.4f\twhat SDR content is multiplied by (read only)",
                      static_cast<double>(v->plan.sdr_scale));
        return MP_OK;
    case 8:
        std::snprintf(out, out_bytes, "frames\t%llu\tpresented so far (read only)",
                      static_cast<unsigned long long>(v->frames));
        return MP_OK;
    case 9:
        std::snprintf(out, out_bytes,
                      "precision\t%s\twhat it renders into; fp16 is what a display gets "
                      "and all DXGI will present",
                      v->window == nullptr && v->wide_target ? "fp32" : "fp16");
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes,
                      "convert\t%s\twhat the source transfer needs (read only)",
                      mp::video::name_of(v->plan.convert));
        return MP_OK;
    case 11:
        std::snprintf(out, out_bytes, "trouble\t%s\twhat went wrong (read only)",
                      v->trouble.empty() ? "nothing" : v->trouble.c_str());
        return MP_OK;
    default:
        break;
    }
    return MP_END;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

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
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

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
// the screenshot people want anyway.
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
// What is not here yet, and why: NV12 and P010 are declared by the ABI and
// refused by this module, because nothing in this tree decodes video and a
// conversion path with no producer is a path no test can check. The tone
// mappers are chosen and reported and not yet applied, for the same reason --
// §9.3's `driver` provider drives the GPU's video processor over a decoded
// surface, and there are no decoded surfaces. Both arrive with the decoder.

#include "colour_plan.hpp"

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
Texture2D<float4> source : register(t0);
SamplerState bilinear : register(s0);

cbuffer Constants : register(b0)
{
    float sdr_scale;   // §9.6: the display's white over scRGB's 80 nits
    float3 padding;
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

// The sRGB electro-optical transfer function, piecewise as the standard states
// it. Not a 2.2 power: that is the approximation §9.2 records Windows using in
// the one place it should not, and this file is not going to repeat it.
float3 srgb_to_linear(float3 c)
{
    // Clamped, then abs: a UNORM texture cannot be negative, but the compiler
    // cannot prove it and pow of a negative base is undefined -- which is what
    // X3571 says, and this file is built with warnings as errors precisely so
    // that a shader nobody reads cannot quietly contain one.
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
}

float4 ps_main(Vertex input) : SV_Target
{
    float4 texel = source.Sample(bilinear, input.uv);
    // **The target is scRGB, which is linear.** An sRGB texture sampled without
    // decoding would be presented as though its gamma were already gone, which
    // is the washed-out picture people mistake for a tone mapping problem.
    float3 linear_rgb = srgb_to_linear(texel.rgb) * sdr_scale;
    return float4(linear_rgb, texel.a);
}
)HLSL";

struct Constants {
    float sdr_scale = 1.0f;
    float padding[3] = {0.0f, 0.0f, 0.0f};
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
    Com<ID3D11PixelShader> pixel_shader;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11Buffer> constants;

    /// The frame most recently uploaded.
    Com<ID3D11Texture2D> source;
    Com<ID3D11ShaderResourceView> source_view;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;

    HWND window = nullptr;
    bool warp = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    mp::video::Stream stream{};
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

    const D3D_DRIVER_TYPE first = v->warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = ::D3D11CreateDevice(nullptr, first, nullptr, flags, k_levels,
                                     static_cast<UINT>(std::size(k_levels)),
                                     D3D11_SDK_VERSION, v->device.put(), nullptr,
                                     v->context.put());
#ifndef NDEBUG
    if (FAILED(hr)) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(nullptr, first, nullptr, flags, k_levels,
                                 static_cast<UINT>(std::size(k_levels)), D3D11_SDK_VERSION,
                                 v->device.put(), nullptr, v->context.put());
    }
#endif
    if (FAILED(hr) && !v->warp) {
        // **WARP is the answer rather than the consolation.** A machine with no
        // usable adapter -- a CI runner, a remote session -- still renders, and
        // renders the same pixels every time, which is what makes a hash of
        // them worth anything.
        log_line(MP_LOG_INFO, "video_d3d11: no hardware device, using WARP");
        v->warp = true;
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, k_levels,
                                 static_cast<UINT>(std::size(k_levels)), D3D11_SDK_VERSION,
                                 v->device.put(), nullptr, v->context.put());
    }
    if (FAILED(hr)) {
        char code[64];
        std::snprintf(code, sizeof(code), "no Direct3D 11 device (0x%08lX)",
                      static_cast<unsigned long>(hr));
        why = code;
        return false;
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
    Com<ID3DBlob> ps;
    if (!compile("vs_main", "vs_5_0", vs, why) || !compile("ps_main", "ps_5_0", ps, why)) {
        return false;
    }
    if (FAILED(v->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                             nullptr, v->vertex_shader.put())) ||
        FAILED(v->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                            nullptr, v->pixel_shader.put()))) {
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

DXGI_FORMAT dxgi_format_of(mp::video::SwapFormat f) noexcept
{
    return f == mp::video::SwapFormat::rgb10_hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                   : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

DXGI_COLOR_SPACE_TYPE colour_space_of(mp::video::SwapFormat f) noexcept
{
    // scRGB is linear with the BT.709 primaries; HDR10 is PQ with BT.2020.
    return f == mp::video::SwapFormat::rgb10_hdr10
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

    const DXGI_FORMAT format = dxgi_format_of(v->plan.format);

    if (v->window != nullptr) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = v->width;
        desc.Height = v->height;
        desc.Format = format;
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
            const DXGI_COLOR_SPACE_TYPE space = colour_space_of(v->plan.format);
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

/// The frame's texture, made once and reused while the geometry holds.
bool upload(MpVideo* v, const MpVideoFrame& frame, std::string& why)
{
    if (frame.format != MP_PIXEL_BGRA8) {
        why = "this build presents MP_PIXEL_BGRA8 only; NV12 and P010 arrive with "
              "the decoder that produces them";
        return false;
    }
    if (frame.plane[0] == nullptr || frame.stride[0] < frame.width * 4u) {
        why = "the frame has no pixels, or a stride too short for its width";
        return false;
    }

    if (!v->source || v->source_width != frame.width || v->source_height != frame.height) {
        v->source_view.reset();
        v->source.reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame.width;
        desc.Height = frame.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(v->device->CreateTexture2D(&desc, nullptr, v->source.put())) ||
            FAILED(v->device->CreateShaderResourceView(v->source.get(), nullptr,
                                                       v->source_view.put()))) {
            why = "no texture for the frame";
            return false;
        }
        v->source_width = frame.width;
        v->source_height = frame.height;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(v->context->Map(v->source.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        why = "the frame texture would not map";
        return false;
    }
    const auto* src = static_cast<const std::uint8_t*>(frame.plane[0]);
    auto* dst = static_cast<std::uint8_t*>(mapped.pData);
    const std::size_t row = static_cast<std::size_t>(frame.width) * 4u;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                    src + static_cast<std::size_t>(y) * frame.stride[0], row);
    }
    v->context->Unmap(v->source.get(), 0);
    return true;
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

    Constants constants{};
    constants.sdr_scale = v->plan.sdr_scale;
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
    ID3D11ShaderResourceView* resources[] = {v->source_view.get()};
    ID3D11SamplerState* samplers[] = {v->sampler.get()};
    ID3D11Buffer* buffers[] = {v->constants.get()};

    v->context->OMSetRenderTargets(1, views, nullptr);
    v->context->RSSetViewports(1, &viewport);
    v->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    v->context->IASetInputLayout(nullptr); // the triangle comes from SV_VertexID
    v->context->VSSetShader(v->vertex_shader.get(), nullptr, 0);
    v->context->PSSetShader(v->pixel_shader.get(), nullptr, 0);
    v->context->PSSetShaderResources(0, 1, resources);
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

MpResult MP_CALL video_read_back(MpVideo* v, void* dst, std::size_t dst_bytes,
                                 std::uint32_t* out_width,
                                 std::uint32_t* out_height) noexcept
try {
    if (v == nullptr || out_width == nullptr || out_height == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_width = v->width;
    *out_height = v->height;
    const std::size_t needed = static_cast<std::size_t>(v->width) * v->height * 4u;
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

    // **Back to 8-bit sRGB, which is what a hash and a screenshot both want.**
    // The target is linear FP16 or PQ 10-bit; neither is a thing to write to a
    // PNG or to compare by eye. The inverse of the shader's own decode, so a
    // frame that went in unscaled comes back out unchanged.
    auto* out = static_cast<std::uint8_t*>(dst);
    const bool half = v->plan.format == mp::video::SwapFormat::fp16_scrgb;
    for (std::uint32_t y = 0; y < v->height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                          static_cast<std::size_t>(y) * mapped.RowPitch;
        for (std::uint32_t x = 0; x < v->width; ++x) {
            float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (half) {
                const auto* texel = reinterpret_cast<const std::uint16_t*>(row) + x * 4u;
                for (int c = 0; c < 4; ++c) {
                    // A minimal half-to-float, because DirectXMath is a
                    // dependency this module does not otherwise need.
                    const std::uint16_t h = texel[c];
                    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
                    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
                    const std::uint32_t mantissa = h & 0x3FFu;
                    std::uint32_t bits = 0;
                    if (exponent == 0) {
                        bits = sign; // zero, or a subnormal near enough to it
                    } else if (exponent == 31) {
                        bits = sign | 0x7F800000u | (mantissa << 13);
                    } else {
                        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
                    }
                    float value = 0.0f;
                    std::memcpy(&value, &bits, sizeof(value));
                    rgba[c] = value;
                }
            } else {
                const auto* texel = reinterpret_cast<const std::uint32_t*>(row) + x;
                const std::uint32_t packed = *texel;
                rgba[0] = static_cast<float>(packed & 0x3FFu) / 1023.0f;
                rgba[1] = static_cast<float>((packed >> 10) & 0x3FFu) / 1023.0f;
                rgba[2] = static_cast<float>((packed >> 20) & 0x3FFu) / 1023.0f;
                rgba[3] = static_cast<float>((packed >> 30) & 0x3u) / 3.0f;
            }

            const auto encode = [half](float linear) {
                if (!half) {
                    return linear; // PQ, and undoing that is the tone mapper's job
                }
                const float clamped = std::clamp(linear, 0.0f, 1.0f);
                return clamped <= 0.0031308f
                           ? clamped * 12.92f
                           : 1.055f * std::powf(clamped, 1.0f / 2.4f) - 0.055f;
            };
            const auto byte = [](float v01) {
                return static_cast<std::uint8_t>(std::clamp(v01, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            std::uint8_t* pixel =
                out + (static_cast<std::size_t>(y) * v->width + x) * 4u;
            pixel[0] = byte(encode(rgba[2])); // B
            pixel[1] = byte(encode(rgba[1])); // G
            pixel[2] = byte(encode(rgba[0])); // R
            pixel[3] = byte(rgba[3]);
        }
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
        if (std::strcmp(value, "warp") == 0) {
            v->warp = true;
        } else if (std::strcmp(value, "hardware") == 0) {
            v->warp = false;
        } else {
            return MP_ERR_INVALID;
        }
        return v->device ? MP_ERR_UNSUPPORTED : MP_OK;
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
        std::snprintf(out, out_bytes, "format\t%s\tthe swap chain (read only)",
                      mp::video::name_of(v->plan.format));
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
    /* set       */ &video_set,
    /* describe  */ &video_describe,
    /* read_back */ &video_read_back,
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

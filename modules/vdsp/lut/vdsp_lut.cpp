// SPDX-License-Identifier: GPL-3.0-or-later
//
// A 3D lookup table, applied on the presenter's device.
//
// **The first stage of §9.8.3's kind, and the reason the kind exists.** The
// video engine is meant to carry colour grading, and a cube LUT is what grading
// actually ships: every tool writes one, most read nothing else, and a table is
// the one colour transform that can be held to a number without a reference --
// an identity table must give the picture back.
//
// **The interpolation is in the shader and not in a sampler**, which is the
// decision this file turns on. A `Texture3D` sampled with
// `D3D11_FILTER_MIN_MAG_MIP_LINEAR` is interpolated by fixed-function hardware
// whose subtexel precision the specification does not state -- it is commonly
// eight bits, it differs between vendors, and it is not something a test can be
// written against. A colour transform that comes out differently on two GPUs is
// not a colour transform. So the corners are point-fetched with `Load` and
// combined in fp32 arithmetic this file can be read against.
//
// **Tetrahedral rather than trilinear**, for a property rather than a
// preference: on the neutral axis, where R = G = B, a tetrahedral combination
// lands exactly on the diagonal of the cell and a trilinear one does not. Greys
// stay grey. It is also four fetches instead of eight, which is the direction a
// cheaper answer usually is not.

#include "cube.hpp"
#include "module_log.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

/// The host, for the log. Null when nobody passed one, which is what a test
/// harness usually does and which `mp::log::line` answers by doing nothing.
const MpHost* g_host = nullptr;

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

/// The smallest owning pointer that does the job, as `video_d3d11` has.
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    T** put() noexcept
    {
        reset();
        return &raw_;
    }
    [[nodiscard]] T* get() const noexcept { return raw_; }
    T* operator->() const noexcept { return raw_; }
    explicit operator bool() const noexcept { return raw_ != nullptr; }
    void reset() noexcept
    {
        if (raw_ != nullptr) {
            raw_->Release();
            raw_ = nullptr;
        }
    }
    void attach(T* p) noexcept
    {
        reset();
        raw_ = p;
    }

private:
    T* raw_ = nullptr;
};

constexpr char k_shader[] = R"HLSL(
Texture3D<float4> table  : register(t0);
Texture2D<float4> source : register(t1);
SamplerState point_clamp : register(s0);

cbuffer Grade : register(b0)
{
    float3 domain_min;
    float  size;        // entries per axis, as a float because it is divided by
    float3 domain_span; // domain_max - domain_min, precomputed
    float  strength;    // 0 is the input, 1 is the table. Between is a mix.
};

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID)
{
    Vertex out_vertex;
    out_vertex.uv = float2((id << 1) & 2, id & 2);
    out_vertex.position = float4(out_vertex.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return out_vertex;
}

float3 corner(int3 at)
{
    // Clamped rather than wrapped: the cell above the last one does not exist,
    // and reading round to the first would map white onto black.
    int last = int(size) - 1;
    return table.Load(int4(clamp(at, int3(0, 0, 0), int3(last, last, last)), 0)).rgb;
}

float3 tetrahedral(float3 rgb)
{
    // Into the table's own domain, and clamped there. A value outside it has no
    // entry -- a table over 0..1 shown scene light above one has nothing to say
    // about it -- and a LUT meant for high dynamic range states DOMAIN_MAX.
    float3 t = saturate((rgb - domain_min) / domain_span) * (size - 1.0);
    int3 i = int3(floor(t));
    float3 f = t - float3(i);

    float3 c000 = corner(i);
    float3 c111 = corner(i + int3(1, 1, 1));
    float3 a, b, c;

    if (f.r > f.g) {
        if (f.g > f.b) {          // r > g > b
            a = corner(i + int3(1, 0, 0)) - c000;
            b = corner(i + int3(1, 1, 0)) - corner(i + int3(1, 0, 0));
            c = c111 - corner(i + int3(1, 1, 0));
        } else if (f.r > f.b) {   // r > b > g
            a = corner(i + int3(1, 0, 0)) - c000;
            b = c111 - corner(i + int3(1, 0, 1));
            c = corner(i + int3(1, 0, 1)) - corner(i + int3(1, 0, 0));
        } else {                  // b > r > g
            a = corner(i + int3(1, 0, 1)) - corner(i + int3(0, 0, 1));
            b = c111 - corner(i + int3(1, 0, 1));
            c = corner(i + int3(0, 0, 1)) - c000;
        }
    } else {
        if (f.b > f.g) {          // b > g > r
            a = c111 - corner(i + int3(0, 1, 1));
            b = corner(i + int3(0, 1, 1)) - corner(i + int3(0, 0, 1));
            c = corner(i + int3(0, 0, 1)) - c000;
        } else if (f.b > f.r) {   // g > b > r
            a = c111 - corner(i + int3(0, 1, 1));
            b = corner(i + int3(0, 1, 0)) - c000;
            c = corner(i + int3(0, 1, 1)) - corner(i + int3(0, 1, 0));
        } else {                  // g > r > b
            a = corner(i + int3(1, 1, 0)) - corner(i + int3(0, 1, 0));
            b = corner(i + int3(0, 1, 0)) - c000;
            c = c111 - corner(i + int3(1, 1, 0));
        }
    }
    return c000 + a * f.r + b * f.g + c * f.b;
}

float4 ps_main(Vertex input) : SV_Target
{
    float4 texel = source.Sample(point_clamp, input.uv);
    return float4(lerp(texel.rgb, tetrahedral(texel.rgb), strength), texel.a);
}
)HLSL";

struct Constants {
    float domain_min[3] = {0.0f, 0.0f, 0.0f};
    float size = 2.0f;
    float domain_span[3] = {1.0f, 1.0f, 1.0f};
    float strength = 1.0f;
};

} // namespace

struct MpVideoDsp {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11VertexShader> vertex_shader;
    Com<ID3D11PixelShader> pixel_shader;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;

    /// The table, as a 3D texture. Identity until somebody names a file, so a
    /// stage that is in the chain and has not been given one is a stage that
    /// changes nothing rather than one that produces black.
    Com<ID3D11Texture3D> table;
    Com<ID3D11ShaderResourceView> table_view;
    mp::CubeLut lut;
    std::string file;
    float strength = 1.0f;

    /// The picture, and what it is drawn into.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Com<ID3D11Texture2D> target;
    Com<ID3D11RenderTargetView> target_view;
    /// A view over whatever texture the last `process` was given. Remade when
    /// the texture changes, which for a presenter's intermediate is never after
    /// the first frame.
    Com<ID3D11ShaderResourceView> source_view;
    void* source_texture = nullptr;

    std::string trouble;
};

namespace {

bool make_shaders(MpVideoDsp* d, std::string& why)
{
    // The same flags `video_d3d11` compiles with, and for the same reason: no
    // partial precision anywhere near a colour transform.
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS;

    Com<ID3DBlob> code;
    Com<ID3DBlob> errors;
    if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, "vdsp_lut", nullptr, nullptr,
                          "vs_main", "vs_5_0", flags, 0, code.put(), errors.put()))) {
        why = errors ? static_cast<const char*>(errors->GetBufferPointer())
                     : "the vertex shader would not compile";
        return false;
    }
    if (FAILED(d->device->CreateVertexShader(code->GetBufferPointer(),
                                             code->GetBufferSize(), nullptr,
                                             d->vertex_shader.put()))) {
        why = "no vertex shader";
        return false;
    }
    if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, "vdsp_lut", nullptr, nullptr,
                          "ps_main", "ps_5_0", flags, 0, code.put(), errors.put()))) {
        why = errors ? static_cast<const char*>(errors->GetBufferPointer())
                     : "the pixel shader would not compile";
        return false;
    }
    if (FAILED(d->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                            nullptr, d->pixel_shader.put()))) {
        why = "no pixel shader";
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d->device->CreateBuffer(&buffer, nullptr, d->constants.put()))) {
        why = "no constant buffer";
        return false;
    }

    // **Point, not linear.** The table is fetched with `Load` and never through
    // this; what it samples is the picture, one texel per pixel, at the same
    // size. A linear filter here would blur a frame that is not being resized.
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(d->device->CreateSamplerState(&sampler, d->sampler.put()))) {
        why = "no sampler";
        return false;
    }
    return true;
}

/// The table, uploaded. **Four channels, because Direct3D has no three-channel
/// float format** -- the alpha is written and never read.
bool upload_table(MpVideoDsp* d, std::string& why)
{
    d->table_view.reset();
    d->table.reset();

    const std::uint32_t n = d->lut.size;
    std::vector<float> rgba(static_cast<std::size_t>(n) * n * n * 4);
    for (std::size_t i = 0, at = 0; i < rgba.size(); i += 4, at += 3) {
        rgba[i + 0] = d->lut.table[at + 0];
        rgba[i + 1] = d->lut.table[at + 1];
        rgba[i + 2] = d->lut.table[at + 2];
        rgba[i + 3] = 1.0f;
    }

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = n;
    desc.Height = n;
    desc.Depth = n;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = rgba.data();
    initial.SysMemPitch = n * 4u * sizeof(float);
    initial.SysMemSlicePitch = n * n * 4u * sizeof(float);

    if (FAILED(d->device->CreateTexture3D(&desc, &initial, d->table.put()))) {
        why = "the table would not become a texture";
        return false;
    }
    if (FAILED(d->device->CreateShaderResourceView(d->table.get(), nullptr,
                                                   d->table_view.put()))) {
        why = "the table would take no view";
        return false;
    }
    return true;
}

MpResult MP_CALL lut_probe(MpGraphicsApi api, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // **`api` is the question** (§9.8.3). This runs on the presenter's D3D11
    // device or it does not run: a system-memory answer would be the round trip
    // §9.8.1 exists to avoid, and saying zero is how a host learns that before
    // it opens anything.
    *out_score = api == MP_GRAPHICS_D3D11 ? 80u : 0u;
    return MP_OK;
}

MpResult MP_CALL lut_open(const MpGraphicsDevice* device, MpVideoDsp** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    if (device == nullptr || device->size < sizeof(MpGraphicsDevice) ||
        device->api != MP_GRAPHICS_D3D11 || device->device == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }

    auto d = std::unique_ptr<MpVideoDsp>(new (std::nothrow) MpVideoDsp());
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    // The presenter's, not one of ours. Retained because this outlives the call
    // and the presenter is entitled to forget it lent it.
    auto* raw = static_cast<ID3D11Device*>(device->device);
    raw->AddRef();
    d->device.attach(raw);
    d->device->GetImmediateContext(d->context.put());

    std::string why;
    if (!make_shaders(d.get(), why)) {
        mp::log::line(g_host, MP_LOG_WARN, ("vdsp_lut: " + why).c_str());
        return MP_ERR_UNSUPPORTED;
    }
    // **Identity until somebody names a file.** A stage in the chain with
    // nothing loaded changes nothing, which is what it should do; producing
    // black would make an empty setting look like a broken pipeline.
    d->lut = mp::identity_cube(2);
    if (!upload_table(d.get(), why)) {
        mp::log::line(g_host, MP_LOG_WARN, ("vdsp_lut: " + why).c_str());
        return MP_ERR_UNSUPPORTED;
    }

    *out = d.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL lut_close(MpVideoDsp* d) noexcept
{
    delete d;
}

MpResult MP_CALL lut_configure(MpVideoDsp* d, const MpVideoInfo* in,
                               MpVideoInfo* out) noexcept
try {
    if (d == nullptr || in == nullptr || out == nullptr || in->width == 0 ||
        in->height == 0) {
        return MP_ERR_INVALID;
    }

    d->width = in->width;
    d->height = in->height;
    d->target_view.reset();
    d->target.reset();
    d->source_view.reset();
    d->source_texture = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = d->width;
    desc.Height = d->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // The intermediate's own format: linear light at single precision, which is
    // what §9.8.3 says the chain runs in and what the presenter hands over.
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->device->CreateTexture2D(&desc, nullptr, d->target.put()))) {
        d->trouble = "no texture to grade into";
        return MP_ERR_UNSUPPORTED;
    }
    if (FAILED(d->device->CreateRenderTargetView(d->target.get(), nullptr,
                                                 d->target_view.put()))) {
        d->trouble = "the texture would take no render target view";
        return MP_ERR_UNSUPPORTED;
    }

    // **A grade is a function of colour and not of geometry**, so what comes
    // out is what went in. A stage that did change the shape would say so here,
    // and that is the whole reason `configure` answers rather than returning
    // MP_OK.
    MpVideoInfo answer{};
    std::memcpy(&answer, in, std::min<std::size_t>(in->size, sizeof(answer)));
    answer.size = out->size;
    std::memcpy(out, &answer, std::min<std::size_t>(out->size, sizeof(answer)));
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL lut_process(MpVideoDsp* d, const MpVideoFrame* in,
                             MpVideoFrame* out) noexcept
try {
    if (d == nullptr || in == nullptr || out == nullptr || !d->target) {
        return MP_ERR_INVALID;
    }
    if (in->texture == nullptr) {
        d->trouble = "this stage grades a texture, and that frame is in system memory";
        return MP_ERR_UNSUPPORTED;
    }

    if (d->source_texture != in->texture) {
        d->source_view.reset();
        auto* texture = static_cast<ID3D11Texture2D*>(in->texture);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        view.Format = desc.Format;
        if (desc.ArraySize > 1) {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.FirstArraySlice = in->texture_index;
            view.Texture2DArray.ArraySize = 1;
        } else {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
        }
        if (FAILED(d->device->CreateShaderResourceView(texture, &view,
                                                       d->source_view.put()))) {
            d->trouble = "that frame's texture would take no view";
            return MP_ERR_UNSUPPORTED;
        }
        d->source_texture = in->texture;
    }

    Constants constants;
    for (int c = 0; c < 3; ++c) {
        constants.domain_min[c] = d->lut.domain_min[c];
        constants.domain_span[c] = d->lut.domain_max[c] - d->lut.domain_min[c];
    }
    constants.size = static_cast<float>(d->lut.size);
    constants.strength = d->strength;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(d->context->Map(d->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                  &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        d->context->Unmap(d->constants.get(), 0);
    }

    const D3D11_VIEWPORT viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(d->width),
                                  static_cast<float>(d->height),
                                  0.0f,
                                  1.0f};
    ID3D11RenderTargetView* targets[] = {d->target_view.get()};
    ID3D11ShaderResourceView* resources[] = {d->table_view.get(), d->source_view.get()};
    ID3D11SamplerState* samplers[] = {d->sampler.get()};
    ID3D11Buffer* buffers[] = {d->constants.get()};

    d->context->OMSetRenderTargets(1, targets, nullptr);
    d->context->RSSetViewports(1, &viewport);
    d->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->context->IASetInputLayout(nullptr);
    d->context->VSSetShader(d->vertex_shader.get(), nullptr, 0);
    d->context->PSSetShader(d->pixel_shader.get(), nullptr, 0);
    d->context->PSSetShaderResources(0, 2, resources);
    d->context->PSSetSamplers(0, 1, samplers);
    d->context->PSSetConstantBuffers(0, 1, buffers);
    d->context->Draw(3, 0);

    // **Unbound before it is handed on.** Whatever reads this next may not
    // sample a texture that is still a render target, and the next stage in the
    // chain is exactly that reader.
    ID3D11RenderTargetView* none[] = {nullptr};
    d->context->OMSetRenderTargets(1, none, nullptr);

    MpVideoFrame answer{};
    std::memcpy(&answer, in, std::min<std::size_t>(in->size, sizeof(answer)));
    answer.size = out->size;
    answer.texture = d->target.get();
    answer.texture_index = 0;
    for (int i = 0; i < 3; ++i) {
        answer.plane[i] = nullptr;
        answer.stride[i] = 0;
    }
    std::memcpy(out, &answer, std::min<std::size_t>(out->size, sizeof(answer)));
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL lut_reset(MpVideoDsp*) noexcept
{
    // A table carries nothing across frames, so a seek asks nothing of it.
    return MP_OK;
}

MpResult MP_CALL lut_set(MpVideoDsp* d, const char* key, const char* value) noexcept
try {
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "file") == 0) {
        if (value[0] == '\0') {
            d->lut = mp::identity_cube(2);
            d->file.clear();
            std::string why;
            return upload_table(d, why) ? MP_OK : MP_ERR_UNSUPPORTED;
        }
        // **The head opens files and the portable half decides what the text
        // means** (§11). A module is not the head, so it reads its own file --
        // but what the bytes mean is `mp::parse_cube`'s, which is portable and
        // is tested with no device.
        std::FILE* file = nullptr;
        if (fopen_s(&file, value, "rb") != 0 || file == nullptr) {
            d->trouble = std::string{value} + " could not be opened";
            return MP_ERR_INVALID;
        }
        std::string text;
        char chunk[4096];
        for (;;) {
            const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
            text.append(chunk, got);
            if (got != sizeof chunk) {
                break;
            }
        }
        const bool bad = std::ferror(file) != 0;
        std::fclose(file);
        if (bad) {
            d->trouble = std::string{value} + " could not be read to the end";
            return MP_ERR_INVALID;
        }

        mp::CubeText read = mp::parse_cube(text, value);
        if (!read.ok) {
            d->trouble = read.why;
            return MP_ERR_INVALID;
        }
        mp::CubeLut was = std::move(d->lut);
        d->lut = std::move(read.lut);
        std::string why;
        if (!upload_table(d, why)) {
            // Put back what worked: a stage that failed to load a file and
            // then graded to black would be worse than one that refused.
            d->lut = std::move(was);
            std::string ignored;
            (void)upload_table(d, ignored);
            d->trouble = why;
            return MP_ERR_UNSUPPORTED;
        }
        d->file = value;
        return MP_OK;
    }
    if (std::strcmp(key, "strength") == 0) {
        char* end = nullptr;
        const double amount = std::strtod(value, &end);
        if (end == value || *end != '\0') {
            d->trouble = "strength is a number, 0 for the picture and 1 for the table";
            return MP_ERR_INVALID;
        }
        // **No range.** A strength above one extrapolates away from the picture
        // and below zero extrapolates the other way; both are things a colourist
        // does on purpose, and refusing them here would be this file having an
        // opinion about somebody's grade.
        d->strength = static_cast<float>(amount);
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL lut_describe(MpVideoDsp* d, std::uint32_t index, char* out,
                              std::uint32_t out_bytes) noexcept
{
    if (d == nullptr || out == nullptr || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes,
                      "file\t%s\ta .cube lookup table; empty is the identity\tpath",
                      d->file.empty() ? "" : d->file.c_str());
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes,
                      "strength\t%.4f\t0 is the picture, 1 is the table, and either "
                      "side of that extrapolates\tnumber step=0.05",
                      static_cast<double>(d->strength));
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes, "title\t%s\twhat the table calls itself (read only)",
                      d->lut.title.empty() ? "(none)" : d->lut.title.c_str());
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "size\t%u\tentries per axis, and %s (read only)", d->lut.size,
                      d->lut.identity(1e-6f) ? "it is the identity"
                                             : "it changes the picture");
        return MP_OK;
    case 4:
        std::snprintf(out, out_bytes,
                      "domain\t%.4f..%.4f\tthe range the table covers; outside it is "
                      "clamped (read only)",
                      static_cast<double>(d->lut.domain_min[0]),
                      static_cast<double>(d->lut.domain_max[0]));
        return MP_OK;
    case 5:
        std::snprintf(out, out_bytes, "trouble\t%s\twhat went wrong (read only)",
                      d->trouble.empty() ? "nothing" : d->trouble.c_str());
        return MP_OK;
    default:
        break;
    }
    return MP_END;
}

const MpVideoDspVtbl g_vtbl = {
    /* size      */ sizeof(MpVideoDspVtbl),
    /* reserved  */ 0,
    /* probe     */ &lut_probe,
    /* open      */ &lut_open,
    /* close     */ &lut_close,
    /* configure */ &lut_configure,
    /* process   */ &lut_process,
    /* reset     */ &lut_reset,
    /* set       */ &lut_set,
    /* describe  */ &lut_describe,
};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VDSP,
    /* priority    */ 100,
    /* id          */ "vdsp_lut",
    /* name        */ "Lookup table (a .cube grade, applied in linear light)",
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
    return host_abi == MP_ABI_VERSION ? &g_desc : nullptr;
}

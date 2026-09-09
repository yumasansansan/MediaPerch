// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scaler, as a stage: the picture resampled to the size the presenter
// would like, one axis at a time, in the light a person chooses, by a kernel
// with every parameter a person can set.
//
// **Why a stage and not a pass in the presenter.** §9.11's resampling lived in
// the presenter's own shader until the HDR10 test patterns asked three things
// of it at once: a choice of kernel for going down that differs from the one
// for going up, every parameter of every kernel, and a second implementation
// to hold the first to. The first two are settings, and a stage is where a
// person's settings live -- a presenter with thirty keys about resampling is
// a presenter that is a scaler with a swap chain attached. The third is what
// the stage ABI is for. What the presenter keeps is a bilinear fetch for a
// chain with no scaler in it, so that removing this node is a picture that is
// softer rather than a picture that is gone.
//
// **The size is asked, not taken.** `configure` arrives with `out` pre-filled
// by the presenter with what it would like to come out -- the target's size
// -- and answers with what this will produce, which is that. Zeros ask for
// nothing, and nothing is the identity: `process` hands the frame back
// untouched and draws nothing, so that a scaler in the chain at one to one
// costs one pointer copy.
//
// **No caps.** A thousand-lobe Lanczos is thousands of taps per output sample
// per axis, and a draw that takes longer than the GPU's timeout resets the
// driver, which the presenter recovers from by rebuilding. The rows say what
// a setting costs; they do not refuse it. That is the tree's rule about its
// user, and the one exception -- denial of service -- is a driver reset a
// person asked for by name.

#include "kernels.hpp"
#include "module_log.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

/// The host, for the log. Null when nobody passed one.
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

/// The light the passes run in. Linear is what a downscale should average;
/// the other two temper what a kernel with negative lobes does at a hard
/// edge, at the cost of the average no longer being of light.
enum class Light : std::uint32_t { linear = 0, gamma = 1, sigmoid = 2 };

const char* name_of(Light light) noexcept
{
    switch (light) {
    case Light::linear:
        return "linear";
    case Light::gamma:
        return "gamma";
    case Light::sigmoid:
        return "sigmoid";
    }
    return "linear";
}

/// **The shader: one axis of the resampling.** Output sample i of `dst`
/// along the axis sits at source position (i + 0.5) * src / dst - 0.5, in
/// source samples; the kernel is centred there, stretched by the downscale
/// factor so that every source sample under the output is counted, and its
/// weights are normalised so that a flat field stays flat whatever the phase.
/// The other axis is carried across untouched, and the edge is clamped.
///
/// The head is here and the kernels are pasted in after it from
/// `kernels.hpp`; the tail, below, is what calls them.
constexpr char k_shader_head[] = R"HLSL(
Texture2D<float4> source : register(t0);

cbuffer Axis : register(b0)
{
    uint  axis;           // 0 across, 1 down
    uint  kind;           // kernels.hpp's Kernel
    uint  lobes;          // Lanczos's
    float b;              // the cubic's

    float c;
    float src;            // samples along the axis, in
    float dst;            // and out
    float antiring;       // 0 leaves a kernel's overshoot, 1 removes it

    uint  light;          // 0 linear, 1 gamma, 2 sigmoid
    uint  encode;         // this pass reads the picture, so it takes it into the light
    uint  decode;         // this pass writes the result, so it brings it back
    float gamma;

    float sigmoid_center;
    float sigmoid_slope;
    float sigmoid_range;  // the linear value at the top of the curve
    float pad;
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
)HLSL";

constexpr char k_shader_tail[] = R"HLSL(
// --------------------------------------------------------------------------
// The light
// --------------------------------------------------------------------------
//
// **Sign-preserving, and unbounded above.** The picture is linear light on
// the source's own primaries and can be far above one -- a PQ highlight is a
// hundred and twenty-five -- and below nought after a gamut move; a curve
// that clamped would crush the one and clip the other. The gamma is a power
// on the magnitude; the sigmoid is mpv's, an inverse logistic over 0..range
// with the curve continued as a straight line outside it, so that nothing is
// lost and the two halves meet.

float3 gamma_encode(float3 v)
{
    return sign(v) * pow(abs(v), 1.0 / gamma);
}

float3 gamma_decode(float3 v)
{
    return sign(v) * pow(abs(v), gamma);
}

float sigmoid_offset()
{
    return 1.0 / (1.0 + exp(sigmoid_slope * sigmoid_center));
}

float sigmoid_scale()
{
    return 1.0 / (1.0 + exp(sigmoid_slope * (sigmoid_center - 1.0))) - sigmoid_offset();
}

// One component: inside 0..1 the inverse logistic, outside it the line. The
// two meet at both ends, so the ends themselves may take either.
float sigmoid_encode_one(float x)
{
    float r = x;
    if (x > 0.0 && x < 1.0) {
        r = sigmoid_center - log(1.0 / (x * sigmoid_scale() + sigmoid_offset()) - 1.0) / sigmoid_slope;
    }
    return r;
}

float sigmoid_decode_one(float y)
{
    float r = y;
    if (y > 0.0 && y < 1.0) {
        r = (1.0 / (1.0 + exp(sigmoid_slope * (sigmoid_center - y))) - sigmoid_offset()) / sigmoid_scale();
    }
    return r;
}

float3 sigmoid_encode(float3 v)
{
    float3 x = v / sigmoid_range;
    return float3(sigmoid_encode_one(x.r), sigmoid_encode_one(x.g), sigmoid_encode_one(x.b));
}

float3 sigmoid_decode(float3 y)
{
    float3 x = float3(sigmoid_decode_one(y.r), sigmoid_decode_one(y.g), sigmoid_decode_one(y.b));
    return x * sigmoid_range;
}

float3 to_light(float3 v)
{
    float3 out_light = v;
    if (light == 1) {
        out_light = gamma_encode(v);
    } else if (light == 2) {
        out_light = sigmoid_encode(v);
    }
    return out_light;
}

float3 from_light(float3 v)
{
    float3 out_linear = v;
    if (light == 1) {
        out_linear = gamma_decode(v);
    } else if (light == 2) {
        out_linear = sigmoid_decode(v);
    }
    return out_linear;
}

// --------------------------------------------------------------------------
// One axis
// --------------------------------------------------------------------------

float4 ps_axis(Vertex input) : SV_Target
{
    int2 p = int2(input.position.xy);
    float ratio = src / dst;
    float stretch = max(ratio, 1.0);
    float i = axis == 0 ? float(p.x) : float(p.y);
    float s = (i + 0.5) * ratio - 0.5;
    float reach = kernel_support(kind, lobes) * stretch;
    int lo = int(ceil(s - reach));
    int hi = int(floor(s + reach));
    int last = int(src) - 1;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weights = 0.0;
    // **Antiringing** (libplacebo's): the two source samples the output sits
    // between -- at a stretch, the samples within one stretched tap -- bound
    // what a kernel with negative lobes may overshoot past.
    float4 low = float4(1e30, 1e30, 1e30, 1e30);
    float4 high = float4(-1e30, -1e30, -1e30, -1e30);
    [loop] for (int j = lo; j <= hi; ++j) {
        float d = (s - float(j)) / stretch;
        float w = kernel_weight(kind, lobes, b, c, d);
        int jc = clamp(j, 0, last);
        int3 at = axis == 0 ? int3(jc, p.y, 0) : int3(p.x, jc, 0);
        float4 texel = source.Load(at);
        if (encode != 0) {
            texel.rgb = to_light(texel.rgb);
        }
        if (w != 0.0) {
            sum += w * texel;
            weights += w;
        }
        if (abs(d) < 1.0) {
            low = min(low, texel);
            high = max(high, texel);
        }
    }
    float4 result = sum / (abs(weights) < 1e-6 ? 1.0 : weights);
    if (antiring != 0.0) {
        result = lerp(result, clamp(result, low, high), antiring);
    }
    if (decode != 0) {
        result.rgb = from_light(result.rgb);
    }
    return result;
}
)HLSL";

/// Mirrors the `cbuffer` above, which HLSL packs in four-float rows.
struct Constants {
    std::uint32_t axis = 0;
    std::uint32_t kind = 9;
    std::uint32_t lobes = 3;
    float b = 1.0f / 3.0f;

    float c = 1.0f / 3.0f;
    float src = 1.0f;
    float dst = 1.0f;
    float antiring = 0.0f;

    std::uint32_t light = 0;
    std::uint32_t encode = 0;
    std::uint32_t decode = 0;
    float gamma = 2.2f;

    float sigmoid_center = 0.75f;
    float sigmoid_slope = 6.5f;
    float sigmoid_range = 1.0f;
    float pad = 0.0f;
};
static_assert(sizeof(Constants) % 16 == 0, "a constant buffer is a whole number of rows");

/// One fp32 texture with a render target view and a shader resource view.
struct Surface {
    Com<ID3D11Texture2D> texture;
    Com<ID3D11RenderTargetView> target;
    Com<ID3D11ShaderResourceView> source;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    void reset() noexcept
    {
        source.reset();
        target.reset();
        texture.reset();
        width = 0;
        height = 0;
    }
};

} // namespace

struct MpVideoDsp {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11VertexShader> vertex_shader;
    Com<ID3D11PixelShader> pixel_axis;
    Com<ID3D11Buffer> constants;

    /// What a person set, or the defaults: one kernel for an axis that grows
    /// and one for an axis that shrinks, because they are different problems.
    /// Lanczos-3 up is the usual choice and sharp; Hermite down has no
    /// negative lobes and cannot ring, which on a field of one-pixel rulers is
    /// the difference between a picture and a moiré of dark haloes.
    mp::kernels::KernelParams up{mp::kernels::Kernel::lanczos, 3, 1.0f / 3.0f, 1.0f / 3.0f};
    mp::kernels::KernelParams down{mp::kernels::Kernel::hermite, 3, 1.0f / 3.0f, 1.0f / 3.0f};
    float antiring = 0.0f;
    Light light = Light::linear;
    float gamma = 2.2f;
    float sigmoid_center = 0.75f;
    float sigmoid_slope = 6.5f;
    float sigmoid_range = 1.0f;

    /// The geometry: what comes in and what goes out, as `configure` settled
    /// them. Equal is the identity.
    std::uint32_t src_width = 0;
    std::uint32_t src_height = 0;
    std::uint32_t dst_width = 0;
    std::uint32_t dst_height = 0;
    /// The horizontal half's result -- the target's width at the source's
    /// height -- and the target. Remade when the geometry changes.
    Surface across;
    Surface target;
    /// A view over whatever texture the last `process` was given. Remade when
    /// the texture changes, which for a presenter's intermediate is never after
    /// the first frame.
    Com<ID3D11ShaderResourceView> source_view;
    void* source_texture = nullptr;
    std::uint32_t source_index = 0;

    /// What the last `process` did, for the rows.
    std::uint32_t passes = 0;
    std::string trouble;
};

namespace {

bool make_shaders(MpVideoDsp* d, std::string& why)
{
    // The same flags `video_d3d11` compiles with, and for the same reason: no
    // partial precision anywhere near a picture.
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS;
    const std::string source = std::string{k_shader_head} + mp::kernels::k_kernel_hlsl +
                               k_shader_tail;

    Com<ID3DBlob> code;
    Com<ID3DBlob> errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "vdsp_scale", nullptr, nullptr,
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
    if (FAILED(D3DCompile(source.data(), source.size(), "vdsp_scale", nullptr, nullptr,
                          "ps_axis", "ps_5_0", flags, 0, code.put(), errors.put()))) {
        why = errors ? static_cast<const char*>(errors->GetBufferPointer())
                     : "the pixel shader would not compile";
        return false;
    }
    if (FAILED(d->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                            nullptr, d->pixel_axis.put()))) {
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
    return true;
}

/// A texture to draw into and read from, fp32 RGBA: the chain's own format,
/// and a half-precision intermediate would put the dark end of a PQ picture
/// into subnormals (§9.10).
bool make_surface(MpVideoDsp* d, std::uint32_t width, std::uint32_t height, Surface& out,
                  std::string& why)
{
    if (out.texture && out.width == width && out.height == height) {
        return true;
    }
    out.reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->device->CreateTexture2D(&desc, nullptr, out.texture.put()))) {
        why = "no texture to resample into";
        return false;
    }
    if (FAILED(d->device->CreateRenderTargetView(out.texture.get(), nullptr,
                                                 out.target.put()))) {
        why = "the texture would take no render target view";
        return false;
    }
    if (FAILED(d->device->CreateShaderResourceView(out.texture.get(), nullptr,
                                                   out.source.put()))) {
        why = "the texture would take no shader resource view";
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}

/// The textures the geometry needs: none at one to one, the target for one
/// axis, both for two.
bool make_surfaces(MpVideoDsp* d, std::string& why)
{
    const bool across = d->src_width != d->dst_width;
    const bool down = d->src_height != d->dst_height;
    if (!across && !down) {
        d->across.reset();
        d->target.reset();
        return true;
    }
    if (across && down) {
        if (!make_surface(d, d->dst_width, d->src_height, d->across, why)) {
            return false;
        }
    } else {
        d->across.reset();
    }
    return make_surface(d, d->dst_width, d->dst_height, d->target, why);
}

/// Which kernel an axis gets: the one for growing, the one for shrinking.
const mp::kernels::KernelParams& kernel_for(const MpVideoDsp* d, std::uint32_t src,
                                            std::uint32_t dst) noexcept
{
    return dst < src ? d->down : d->up;
}

/// Taps per output sample along an axis: the kernel's width, stretched by the
/// downscale factor. What a setting costs, said in the rows.
std::uint32_t taps_for(const MpVideoDsp* d, std::uint32_t src, std::uint32_t dst) noexcept
{
    if (src == dst || dst == 0) {
        return 0;
    }
    const mp::kernels::KernelParams& k = kernel_for(d, src, dst);
    const double stretch = std::max(static_cast<double>(src) / static_cast<double>(dst), 1.0);
    const double reach = static_cast<double>(mp::kernels::support_of(k.kind, k.lobes)) * stretch;
    return static_cast<std::uint32_t>(2.0 * std::ceil(reach));
}

MpResult MP_CALL scale_probe(MpGraphicsApi api, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // **`api` is the question** (§9.8.3), and the answer is the LUT's: this
    // runs on the presenter's D3D11 device or it does not run.
    *out_score = api == MP_GRAPHICS_D3D11 ? 80u : 0u;
    return MP_OK;
}

MpResult MP_CALL scale_open(const MpGraphicsDevice* device, MpVideoDsp** out) noexcept
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
        mp::log::line(g_host, MP_LOG_WARN, ("vdsp_scale: " + why).c_str());
        return MP_ERR_UNSUPPORTED;
    }
    *out = d.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL scale_close(MpVideoDsp* d) noexcept
{
    delete d;
}

MpResult MP_CALL scale_configure(MpVideoDsp* d, const MpVideoInfo* in,
                                 MpVideoInfo* out) noexcept
try {
    if (d == nullptr || in == nullptr || out == nullptr || in->width == 0 ||
        in->height == 0) {
        return MP_ERR_INVALID;
    }
    // **What the presenter would like to come out**, read before anything is
    // written over it. A host that says nothing gets the picture back as it
    // is: zeros ask for the identity.
    const bool asked = out->size >= offsetof(MpVideoInfo, display_width);
    const std::uint32_t want_width = asked ? out->width : 0u;
    const std::uint32_t want_height = asked ? out->height : 0u;

    d->src_width = in->width;
    d->src_height = in->height;
    d->dst_width = want_width != 0 ? want_width : in->width;
    d->dst_height = want_height != 0 ? want_height : in->height;
    d->source_view.reset();
    d->source_texture = nullptr;
    d->passes = 0;
    if (!make_surfaces(d, d->trouble)) {
        return MP_ERR_UNSUPPORTED;
    }

    MpVideoInfo answer{};
    std::memcpy(&answer, in, std::min<std::size_t>(in->size, sizeof(answer)));
    answer.size = out->size;
    answer.width = d->dst_width;
    answer.height = d->dst_height;
    // Square pixels at the target: the box a shell asked for is the box.
    answer.display_width = d->dst_width;
    answer.display_height = d->dst_height;
    std::memcpy(out, &answer, std::min<std::size_t>(out->size, sizeof(answer)));
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

/// One pass: `axis` of `from` into `into`, at `into`'s size.
void draw_axis(MpVideoDsp* d, std::uint32_t axis, ID3D11ShaderResourceView* from,
               const Surface& into, std::uint32_t src, std::uint32_t dst, bool first,
               bool last)
{
    const mp::kernels::KernelParams& k = kernel_for(d, src, dst);
    Constants constants;
    constants.axis = axis;
    constants.kind = static_cast<std::uint32_t>(k.kind);
    constants.lobes = k.lobes;
    constants.b = k.b;
    constants.c = k.c;
    constants.src = static_cast<float>(src);
    constants.dst = static_cast<float>(dst);
    constants.antiring = d->antiring;
    constants.light = static_cast<std::uint32_t>(d->light);
    constants.encode = first && d->light != Light::linear ? 1u : 0u;
    constants.decode = last && d->light != Light::linear ? 1u : 0u;
    constants.gamma = d->gamma;
    constants.sigmoid_center = d->sigmoid_center;
    constants.sigmoid_slope = d->sigmoid_slope;
    constants.sigmoid_range = d->sigmoid_range;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(d->context->Map(d->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                  &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        d->context->Unmap(d->constants.get(), 0);
    }

    const D3D11_VIEWPORT viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(into.width),
                                  static_cast<float>(into.height),
                                  0.0f,
                                  1.0f};
    // The previous pass's target is this pass's source, so it comes off the
    // output before it goes on the input; Direct3D unbinds one of the two
    // silently otherwise.
    ID3D11RenderTargetView* none[] = {nullptr};
    ID3D11ShaderResourceView* unbound[] = {nullptr};
    d->context->OMSetRenderTargets(1, none, nullptr);
    d->context->PSSetShaderResources(0, 1, unbound);
    ID3D11RenderTargetView* targets[] = {into.target.get()};
    ID3D11ShaderResourceView* resources[] = {from};
    ID3D11Buffer* buffers[] = {d->constants.get()};
    d->context->OMSetRenderTargets(1, targets, nullptr);
    d->context->RSSetViewports(1, &viewport);
    d->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->context->IASetInputLayout(nullptr);
    d->context->VSSetShader(d->vertex_shader.get(), nullptr, 0);
    d->context->PSSetShader(d->pixel_axis.get(), nullptr, 0);
    d->context->PSSetShaderResources(0, 1, resources);
    d->context->PSSetConstantBuffers(0, 1, buffers);
    d->context->Draw(3, 0);
    ++d->passes;
}

MpResult MP_CALL scale_process(MpVideoDsp* d, const MpVideoFrame* in,
                               MpVideoFrame* out) noexcept
try {
    if (d == nullptr || in == nullptr || out == nullptr || d->src_width == 0) {
        return MP_ERR_INVALID;
    }
    if (in->texture == nullptr) {
        d->trouble = "this stage resamples a texture, and that frame is in system memory";
        return MP_ERR_UNSUPPORTED;
    }
    // A frame of another size than `configure` said is taken as it is: the
    // presenter reconfigures the chain when the source changes, and a stage
    // that refused the frame in between would be a black frame in between.
    if (in->width != 0 && in->height != 0 &&
        (in->width != d->src_width || in->height != d->src_height)) {
        d->src_width = in->width;
        d->src_height = in->height;
        if (!make_surfaces(d, d->trouble)) {
            return MP_ERR_UNSUPPORTED;
        }
    }
    d->passes = 0;

    // **The identity, and no draw at all.** The frame goes back as it came.
    if (d->src_width == d->dst_width && d->src_height == d->dst_height) {
        std::memcpy(out, in, std::min<std::size_t>(in->size, out->size));
        return MP_OK;
    }

    if (d->source_texture != in->texture || d->source_index != in->texture_index) {
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
        d->source_index = in->texture_index;
    }

    // **One axis at a time.** A separable kernel over W by H taps is W taps
    // and then H, not W times H. An axis at one to one is skipped, not run
    // through an interpolating kernel that would give the same answer at the
    // cost of a pass -- or through a non-interpolating one that would not.
    const bool across = d->src_width != d->dst_width;
    const bool down = d->src_height != d->dst_height;
    if (across && down) {
        draw_axis(d, 0, d->source_view.get(), d->across, d->src_width, d->dst_width, true,
                  false);
        draw_axis(d, 1, d->across.source.get(), d->target, d->src_height, d->dst_height,
                  false, true);
    } else if (across) {
        draw_axis(d, 0, d->source_view.get(), d->target, d->src_width, d->dst_width, true,
                  true);
    } else {
        draw_axis(d, 1, d->source_view.get(), d->target, d->src_height, d->dst_height, true,
                  true);
    }

    // **Unbound before it is handed on.** Whatever reads this next may not
    // sample a texture that is still a render target.
    ID3D11RenderTargetView* none[] = {nullptr};
    d->context->OMSetRenderTargets(1, none, nullptr);

    MpVideoFrame answer{};
    std::memcpy(&answer, in, std::min<std::size_t>(in->size, sizeof(answer)));
    answer.size = out->size;
    answer.width = d->dst_width;
    answer.height = d->dst_height;
    answer.texture = d->target.texture.get();
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

MpResult MP_CALL scale_reset(MpVideoDsp*) noexcept
{
    // A resampler carries nothing across frames, so a seek asks nothing of it.
    return MP_OK;
}

/// A whole number, or nothing.
bool parse_uint(const char* value, std::uint32_t& out) noexcept
{
    char* end = nullptr;
    const unsigned long n = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || n > 0xfffffffful) {
        return false;
    }
    out = static_cast<std::uint32_t>(n);
    return true;
}

bool parse_number(const char* value, float& out) noexcept
{
    char* end = nullptr;
    const double n = std::strtod(value, &end);
    if (end == value || *end != '\0' || std::isnan(n)) {
        return false;
    }
    out = static_cast<float>(n);
    return true;
}

MpResult MP_CALL scale_set(MpVideoDsp* d, const char* key, const char* value) noexcept
try {
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    const bool is_up = std::strncmp(key, "up", 2) == 0 && (key[2] == '\0' || key[2] == '_');
    const bool is_down =
        std::strncmp(key, "down", 4) == 0 && (key[4] == '\0' || key[4] == '_');
    if (is_up || is_down) {
        mp::kernels::KernelParams& k = is_up ? d->up : d->down;
        const char* rest = key + (is_up ? 2 : 4);
        if (rest[0] == '\0') {
            mp::kernels::Kernel kind{};
            if (!mp::kernels::kernel_from_name(value, kind)) {
                d->trouble = mp::kernels::k_kernel_sentence;
                return MP_ERR_INVALID;
            }
            k.kind = kind;
            return MP_OK;
        }
        if (std::strcmp(rest, "_lobes") == 0) {
            // **No ceiling.** One is the floor because a kernel with no lobes
            // has no support and nothing under it; a thousand is thousands
            // of taps and a person's own business.
            std::uint32_t lobes = 0;
            if (!parse_uint(value, lobes) || lobes == 0) {
                d->trouble = "lobes is a whole number from 1 up";
                return MP_ERR_INVALID;
            }
            k.lobes = lobes;
            return MP_OK;
        }
        if (std::strcmp(rest, "_b") == 0 || std::strcmp(rest, "_c") == 0) {
            float number = 0.0f;
            if (!parse_number(value, number)) {
                d->trouble = std::string{key} + " is a number";
                return MP_ERR_INVALID;
            }
            (rest[1] == 'b' ? k.b : k.c) = number;
            return MP_OK;
        }
        return MP_ERR_UNSUPPORTED;
    }
    if (std::strcmp(key, "antiring") == 0) {
        // 0 to 1 is what it means; past either end it extrapolates, which is
        // a number somebody typed and not a reason to refuse.
        float number = 0.0f;
        if (!parse_number(value, number)) {
            d->trouble = "antiring is a number, 0 for none and 1 for all of it";
            return MP_ERR_INVALID;
        }
        d->antiring = number;
        return MP_OK;
    }
    if (std::strcmp(key, "light") == 0) {
        if (std::strcmp(value, "linear") == 0) {
            d->light = Light::linear;
        } else if (std::strcmp(value, "gamma") == 0) {
            d->light = Light::gamma;
        } else if (std::strcmp(value, "sigmoid") == 0) {
            d->light = Light::sigmoid;
        } else {
            d->trouble = "light is linear, gamma or sigmoid";
            return MP_ERR_INVALID;
        }
        return MP_OK;
    }
    if (std::strcmp(key, "gamma") == 0 || std::strcmp(key, "sigmoid_range") == 0) {
        // A power of nought and a range of nought are divisions by it.
        float number = 0.0f;
        if (!parse_number(value, number) || number <= 0.0f) {
            d->trouble = std::string{key} + " is a positive number";
            return MP_ERR_INVALID;
        }
        (key[0] == 'g' ? d->gamma : d->sigmoid_range) = number;
        return MP_OK;
    }
    if (std::strcmp(key, "sigmoid_center") == 0 || std::strcmp(key, "sigmoid_slope") == 0) {
        float number = 0.0f;
        if (!parse_number(value, number)) {
            d->trouble = std::string{key} + " is a number";
            return MP_ERR_INVALID;
        }
        (key[8] == 'c' ? d->sigmoid_center : d->sigmoid_slope) = number;
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

/// The four rows one kernel takes, for `up` and for `down` alike.
MpResult kernel_rows(const MpVideoDsp* d, const char* which, const mp::kernels::KernelParams& k,
                     std::uint32_t index, char* out, std::uint32_t out_bytes) noexcept
{
    const bool up = which[0] == 'u';
    const char* group = up ? "Upscaling" : "Downscaling";
    switch (index) {
    case 0:
        std::snprintf(out, out_bytes,
                      "%s\t%s\tthe kernel for an axis that %s: hermite and box cannot "
                      "ring, the splines ring less than lanczos, mitchell blurs on "
                      "purpose\tenum:%s group=%s",
                      which, mp::kernels::name_of(k.kind), up ? "grows" : "shrinks",
                      mp::kernels::k_kernel_list, group);
        return MP_OK;
    case 1:
        std::snprintf(out, out_bytes,
                      "%s_lobes\t%u\tLanczos lobes: 3 is the usual, 2 softer, 4 sharper "
                      "and ringier. Every lobe is two more taps per output sample per "
                      "axis, and a thousand is thousands -- a draw longer than the GPU "
                      "allows resets the driver, which the presenter recovers "
                      "from\tint min=1 step=1 group=%s when=%s=lanczos",
                      which, k.lobes, group, which);
        return MP_OK;
    case 2:
        std::snprintf(out, out_bytes,
                      "%s_b\t%.4f\tthe cubic's B: 1/3 with C=1/3 is Mitchell, 0 with "
                      "C=1/2 is Catmull-Rom, 1 with C=0 is the B-spline\tnumber "
                      "step=0.05 group=%s when=%s=bicubic",
                      which, static_cast<double>(k.b), group, which);
        return MP_OK;
    case 3:
        std::snprintf(out, out_bytes,
                      "%s_c\t%.4f\tthe cubic's C; more is sharper and rings "
                      "more\tnumber step=0.05 group=%s when=%s=bicubic",
                      which, static_cast<double>(k.c), group, which);
        return MP_OK;
    default:
        break;
    }
    (void)d;
    return MP_END;
}

MpResult MP_CALL scale_describe(MpVideoDsp* d, std::uint32_t index, char* out,
                                std::uint32_t out_bytes) noexcept
{
    if (d == nullptr || out == nullptr || out_bytes < 64) {
        return MP_ERR_INVALID;
    }
    if (index < 4) {
        return kernel_rows(d, "up", d->up, index, out, out_bytes);
    }
    if (index < 8) {
        return kernel_rows(d, "down", d->down, index - 4, out, out_bytes);
    }
    switch (index) {
    case 8:
        std::snprintf(out, out_bytes,
                      "antiring\t%.4f\thow far a result is pulled back inside the range "
                      "of the source samples it sits between: 0 leaves a kernel's "
                      "overshoot, 1 removes it\tnumber min=0 max=1 step=0.05 group=Ringing",
                      static_cast<double>(d->antiring));
        return MP_OK;
    case 9:
        std::snprintf(out, out_bytes,
                      "light\t%s\twhat the passes average: linear light, a gamma curve, "
                      "or a sigmoid that tempers ringing at hard edges\tenum:linear,gamma,"
                      "sigmoid group=Light",
                      name_of(d->light));
        return MP_OK;
    case 10:
        std::snprintf(out, out_bytes,
                      "gamma\t%.4f\tthe power, when light=gamma\tnumber step=0.1 "
                      "group=Light when=light=gamma",
                      static_cast<double>(d->gamma));
        return MP_OK;
    case 11:
        std::snprintf(out, out_bytes,
                      "sigmoid_center\t%.4f\twhere the curve is steepest, 0 to 1\tnumber "
                      "step=0.05 group=Light when=light=sigmoid",
                      static_cast<double>(d->sigmoid_center));
        return MP_OK;
    case 12:
        std::snprintf(out, out_bytes,
                      "sigmoid_slope\t%.4f\thow steep; more tempers more\tnumber step=0.5 "
                      "group=Light when=light=sigmoid",
                      static_cast<double>(d->sigmoid_slope));
        return MP_OK;
    case 13:
        std::snprintf(out, out_bytes,
                      "sigmoid_range\t%.4f\tthe linear value at the top of the curve: 1 is "
                      "SDR white on an SDR display; above it the curve is "
                      "straight\tnumber step=0.5 group=Light when=light=sigmoid",
                      static_cast<double>(d->sigmoid_range));
        return MP_OK;
    case 14:
        if (d->src_width == 0) {
            std::snprintf(out, out_bytes, "scale\tno picture yet\tsource to target, and "
                                          "the factor per axis (read only)");
        } else if (d->src_width == d->dst_width && d->src_height == d->dst_height) {
            std::snprintf(out, out_bytes,
                          "scale\t%ux%u 1:1\tsource to target: one to one, handed back "
                          "untouched (read only)",
                          d->src_width, d->src_height);
        } else {
            std::snprintf(out, out_bytes,
                          "scale\t%ux%u -> %ux%u, x%.3f across, x%.3f down\tsource to "
                          "target, and the factor per axis (read only)",
                          d->src_width, d->src_height, d->dst_width, d->dst_height,
                          static_cast<double>(d->dst_width) / static_cast<double>(d->src_width),
                          static_cast<double>(d->dst_height) /
                              static_cast<double>(d->src_height));
        }
        return MP_OK;
    case 15: {
        const std::uint32_t across = taps_for(d, d->src_width, d->dst_width);
        const std::uint32_t down = taps_for(d, d->src_height, d->dst_height);
        std::snprintf(out, out_bytes,
                      "cost\t%u taps across, %u down, %u pass%s\tper output sample and "
                      "per frame: the kernel's width stretched by the downscale factor, "
                      "and the draws the last frame took (read only)",
                      across, down, d->passes, d->passes == 1 ? "" : "es");
        return MP_OK;
    }
    case 16:
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
    /* probe     */ &scale_probe,
    /* open      */ &scale_open,
    /* close     */ &scale_close,
    /* configure */ &scale_configure,
    /* process   */ &scale_process,
    /* reset     */ &scale_reset,
    /* set       */ &scale_set,
    /* describe  */ &scale_describe,
};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VDSP,
    /* priority    */ 100,
    /* id          */ "vdsp_scale",
    /* name        */ "Scaler (the picture resampled to the window, any kernel)",
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

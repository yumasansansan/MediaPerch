// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scaler stage's shader, for modules/vdsp/scale/vdsp_scale.cpp: compiled
// with the build by cmake/Shaders.cmake, and carried in the module as the
// bytecode of each entry point.
//
// **The shader: one axis of the resampling.** Output sample i of `dst`
// along the axis sits at source position (i + 0.5) * src / dst - 0.5, in
// source samples; the kernel is centred there, stretched by the downscale
// factor so that every source sample under the output is counted, and its
// weights are normalised so that a flat field stays flat whatever the phase.
// The other axis is carried across untouched, and the edge is clamped.
//
// **The weights are the host's** (`axis_table`, from kernels.hpp's
// `taps_at`, in double): one run of `taps` for every output sample along the
// axis, and where each run starts. What the shader does per tap is a fetch,
// the light and a multiply-add, in a loop of fixed length: eight, unrolled,
// or as many eights as the longest run needs.

Texture2D<float4> source : register(t0);
// **The weights and where they start**, for every output sample along the
// axis: `taps` weights each, normalised, and zero past the sample's own taps;
// and the first source sample, the first and last tap within one stretched
// tap of the centre -- what antiringing bounds the result by -- and how many
// taps are the sample's own.
StructuredBuffer<float> weights : register(t1);
StructuredBuffer<int4>  starts  : register(t2);

cbuffer Axis : register(b0)
{
    uint  axis;           // 0 across, 1 down
    uint  taps;           // weights per output sample, a multiple of eight
    float src;            // samples along the axis, in
    float antiring;       // 0 leaves a kernel's overshoot, 1 removes it

    uint  light;          // 0 linear, 1 gamma, 2 sigmoid
    uint  encode;         // this pass reads the picture, so it takes it into the light
    uint  decode;         // this pass writes the result, so it brings it back
    float gamma;

    // **The curves' constants, worked out on the host in double**: one over
    // the gamma; the sigmoid's centre and slope and one over the slope; its
    // range and one over that; and the logistic at 0, its span to 1, and one
    // over the span -- two exponentials the shader took at every component of
    // every tap.
    float gamma_inv;
    float sigmoid_center;
    float sigmoid_slope;
    float sigmoid_slope_inv;

    float sigmoid_range;
    float sigmoid_range_inv;
    float sigmoid_offset;
    float sigmoid_scale;

    float sigmoid_scale_inv;
    float pad_a;
    float pad_b;
    float pad_c;
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
    return sign(v) * pow(abs(v), gamma_inv);
}

float3 gamma_decode(float3 v)
{
    return sign(v) * pow(abs(v), gamma);
}

// One component: inside 0..1 the inverse logistic, outside it the line. The
// two meet at both ends, so the ends themselves may take either.
float sigmoid_encode_one(float x)
{
    float r = x;
    if (x > 0.0 && x < 1.0) {
        r = sigmoid_center - log(1.0 / (x * sigmoid_scale + sigmoid_offset) - 1.0) * sigmoid_slope_inv;
    }
    return r;
}

float sigmoid_decode_one(float y)
{
    float r = y;
    if (y > 0.0 && y < 1.0) {
        r = (1.0 / (1.0 + exp(sigmoid_slope * (sigmoid_center - y))) - sigmoid_offset) * sigmoid_scale_inv;
    }
    return r;
}

float3 sigmoid_encode(float3 v)
{
    float3 x = v * sigmoid_range_inv;
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

// One tap: the source sample `t` taps past the run's first, in the light,
// weighed into the sum -- unless its weight is nothing, which a tap past the
// sample's own run always is -- and into the bounds when it is within one
// stretched tap of the centre. **Antiringing** (libplacebo's): the source
// samples the output sits between bound what a kernel with negative lobes
// may overshoot past.
void take(int t, int2 p, int4 start, uint base, int last, inout float4 sum, inout float4 low,
          inout float4 high)
{
    int jc = clamp(start.x + t, 0, last);
    int3 at = axis == 0 ? int3(jc, p.y, 0) : int3(p.x, jc, 0);
    float4 texel = source.Load(at);
    if (encode != 0) {
        texel.rgb = to_light(texel.rgb);
    }
    float w = weights[base + uint(t)];
    if (w != 0.0) {
        sum += w * texel;
    }
    if (t >= start.y && t <= start.z) {
        low = min(low, texel);
        high = max(high, texel);
    }
}

float4 ps_axis(Vertex input) : SV_Target
{
    int2 p = int2(input.position.xy);
    int i = axis == 0 ? p.x : p.y;
    int4 start = starts[i];
    uint base = uint(i) * taps;
    int last = int(src) - 1;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float4 low = float4(1e30, 1e30, 1e30, 1e30);
    float4 high = float4(-1e30, -1e30, -1e30, -1e30);
    if (taps == 8) {
        [unroll] for (int t = 0; t < 8; ++t) {
            take(t, p, start, base, last, sum, low, high);
        }
    } else {
        [loop] for (uint g = 0; g < taps; g += 8) {
            [unroll] for (int u = 0; u < 8; ++u) {
                take(int(g) + u, p, start, base, last, sum, low, high);
            }
        }
    }
    float4 result = sum;
    if (antiring != 0.0) {
        result = lerp(result, clamp(result, low, high), antiring);
    }
    if (decode != 0) {
        result.rgb = from_light(result.rgb);
    }
    return result;
}

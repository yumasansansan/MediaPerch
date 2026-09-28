// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LUT stage's shader, for modules/vdsp/lut/vdsp_lut.cpp: the picture's
// colour looked up in a 3D table, interpolated tetrahedrally in the table's
// own domain. Compiled with the build by cmake/Shaders.cmake, and carried in
// the module as the bytecode of each entry point.

Texture3D<float4> table  : register(t0);
Texture2D<float4> source : register(t1);
SamplerState point_clamp : register(s0);

cbuffer Grade : register(b0)
{
    float3 domain_min;
    float  size;         // entries per axis, as a float because it bounds a float
    float3 domain_scale; // (size - 1) / (domain_max - domain_min), worked out in double
    float  strength;     // 0 is the input, 1 is the table. Between is a mix.
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
    // One multiplication into table steps and then the clamp, which is the
    // same thing as the clamp to the domain and the steps after it: a division
    // at every pixel was two and a half units in the last place, as Direct3D
    // allows it.
    float3 t = clamp((rgb - domain_min) * domain_scale, 0.0, size - 1.0);
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

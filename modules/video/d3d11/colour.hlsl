// SPDX-License-Identifier: GPL-3.0-or-later
//
// The presenter's colour shader, for modules/video/d3d11/video_d3d11.cpp:
// compiled with the build by cmake/Shaders.cmake, and carried in the module
// as the bytecode of each entry point.
//
// **One triangle, not two.** A full-screen triangle covering the viewport
// wastes a quarter of its area and rasterises with no diagonal seam, where a
// quad has one down the middle that shows up in exactly the gradients this is
// for. The vertex shader makes it from `SV_VertexID` and there is no vertex
// buffer at all.
//
// The pixel shaders are §9's colour pipeline and, since the HDR10 test
// patterns, the chroma reconstruction in front of it: chroma reconstructed at
// each luma sample by a kernel and the stream's siting, the transfer undone
// into linear light at the source's own size, then -- after §9.8.3's chain,
// which is where the picture is resampled to the target, by a stage (§9.11)
// -- the roll-off, the gamut and the encoding. Writing it as shaders rather
// than as lookups is what lets each of those be a formula a test can hold to
// a standard.
//
// **The kernels' weights come from a table.** The chroma passes read the
// weights `modules/shared/kernels` works out in double for each phase, so that
// this and the scaler stage share one formula set and neither evaluates a
// kernel per pixel.

Texture2D<float4> rgba     : register(t0);  // the BGRA8 path
Texture2D<float>  luma     : register(t1);  // Y, semi-planar and planar alike
Texture2D<float2> chroma   : register(t2);  // semi-planar CbCr, interleaved
Texture2D<float>  chroma_u : register(t3);  // planar Cb
Texture2D<float>  chroma_v : register(t4);  // planar Cr
// **Chroma at the luma's width, still at the chroma's height**: what
// `ps_chroma_across` writes and the main pass finishes downwards. The
// reconstruction is separable, so it is done as two passes of six taps
// rather than one of thirty-six -- which on an Iris Xe at 4K60 was the
// difference between a fifth of the frames dropped and almost none.
Texture2D<float2> chroma_wide : register(t5);
// **The chroma reconstruction's weights**: four runs of `chroma_taps`, across
// at an even luma column and at an odd one, then down at an even luma row and
// at an odd one -- the two phases a subsampled axis has. Normalised, and zero
// past each run's own taps.
StructuredBuffer<float> chroma_weights : register(t6);
SamplerState bilinear : register(s0);

cbuffer Constants : register(b0)
{
    float sdr_scale;      // §9.6: the display's white over scRGB's 80 nits
    float luma_offset;    // studio range puts black at 16/255
    float luma_scale;
    float chroma_scale;

    float4 yuv;           // r_v, g_u, g_v, b_u -- see yuv_matrix.hpp

    float sample_scale;   // mp_pixel_sample_scale: where the bits sit, not how many
    float transfer_gamma; // 2.4 for BT.1886, used only by transfer_kind 1
    // **An integer, because it is a name and not a quantity.** The floats
    // around it are multipliers -- `has_chroma` multiplies the centred chroma
    // so that 4:0:0 comes out grey without a branch, and `sample_scale` scales
    // samples -- and a float there is branch-free arithmetic rather than
    // imitation. This one is compared, never multiplied, so it is a uint.
    uint  transfer_kind;  // 0 sRGB, 1 a pure power, 2 PQ, 3 HLG
    float has_chroma;     // 0 for 4:0:0, where there is no chroma plane to read

    float hlg_peak;       // §9.9.1: the display's peak, which the OOTF scales by
    float tone_peak;      // where the roll-off aims, in nits -- and the unit the
                          // output is in when it maps -- or 0 for no tone mapping
    // **Write HDR10 instead of scRGB.** A provider that is not ours does its
    // mapping in its own pass and wants what a display would be sent: PQ on
    // BT.2020 primaries. So the shader stops one step short -- it still
    // decodes, and it neither maps nor moves the gamut.
    uint  emit_pq;
    // **§9.8.3's two halves, as two flags.** With a chain in the path the
    // shader runs twice: once stopping at linear light, which is what grading
    // is defined on, and once starting from it. With no chain both are zero
    // and this is the single pass it has always been.
    uint  emit_linear;

    uint  linear_in;
    float hlg_gamma1;     // the OOTF's system gamma, less one, from hlg_peak
    uint  gamut_mode;     // past the display's gamut: 0 clip, 1 desaturate at constant luminance
    uint  pad_a;

    // Source primaries to the buffer's, in linear light. Identity unless they
    // differ, which is the usual case and costs three dots either way.
    float4 gamut0;
    float4 gamut1;
    float4 gamut2;

    // **The chroma reconstruction** (siting.hpp, kernels.hpp). How many chroma
    // samples there are per luma sample, per axis, and the chroma plane's
    // picture size, for the edge clamp; then, for each of the four runs of
    // `chroma_weights`, the first chroma sample it reaches, counted from the
    // one at half the luma sample's position -- where the siting went -- and
    // how long every run is, a multiple of eight.
    float2 chroma_step;
    float2 chroma_size;
    int4   chroma_first;
    uint   chroma_taps;
    uint   chroma_planar; // 1 when Cb and Cr are two planes, 0 when interleaved

    // **BT.2390's EETF, as far as it is the same for every pixel**, worked out
    // in double from where the roll-off starts (the mastering peak) and where
    // it aims: the start as a PQ code value and its reciprocal, the target
    // over the start in PQ, the knee, and one over the span above the knee.
    float  tone_source_pq;
    float  tone_source_inv;
    float  tone_mt;
    float  tone_ks;
    float  tone_span_inv;
    float  tone_peak_inv;  // one over tone_peak: the output's unit, as a factor

    // **The buffer's luminance, as its primaries define it**: the Y row of
    // BT.709's RGB-to-XYZ matrix, worked out in double on the host from the
    // chromaticities the gamut matrix is worked out from. Y'CbCr's 0.2126,
    // 0.7152 and 0.0722 are the same numbers to four decimals, up to 4e-5 off
    // the luminance the gamut matrix keeps.
    float3 luma709;
    float  pad_c;
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
float3 sdr_to_linear(float3 c)
{
    // Clamped, then abs: a UNORM texture cannot be negative, but the compiler
    // cannot prove it and pow of a negative base is undefined -- which is what
    // X3571 says, and this file is built with warnings as errors precisely so
    // that a shader nobody reads cannot quietly contain one.
    c = saturate(c);
    float3 piecewise = c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
    float3 power = pow(abs(c), transfer_gamma);
    return transfer_kind == 0 ? piecewise : power;
}

// ST.2084's constants, as the standard writes them: ratios of small integers,
// not decimals somebody typed.
static const float k_pq_m1 = 0.1593017578125;  // 2610 / 16384
static const float k_pq_m2 = 78.84375;         // 2523 / 4096 * 128
static const float k_pq_c1 = 0.8359375;        // 3424 / 4096
static const float k_pq_c2 = 18.8515625;       // 2413 / 4096 * 32
static const float k_pq_c3 = 18.6875;          // 2392 / 4096 * 32

/// **PQ states absolute nits** (§9.9.1), so this returns them: 0 to 10000.
float3 pq_to_nits(float3 e)
{
    float3 p = pow(abs(saturate(e)), 1.0 / k_pq_m2);
    float3 num = max(p - k_pq_c1, 0.0);
    float3 den = max(k_pq_c2 - k_pq_c3 * p, 1e-6);
    return 10000.0 * pow(abs(num / den), 1.0 / k_pq_m1);
}

float3 nits_to_pq(float3 nits)
{
    float3 y = pow(abs(max(nits, 0.0) / 10000.0), k_pq_m1);
    return pow(abs((k_pq_c1 + k_pq_c2 * y) / (1.0 + k_pq_c3 * y)), k_pq_m2);
}

/// **HLG is scene-referred and PQ is not**, which §9.9.1 is entirely about: the
/// inverse OETF gives a scene signal, and the OOTF turns it into display light
/// with a system gamma derived from *the display's* peak. The same code values
/// are deliberately a different picture on a 600-nit panel and a 1000-nit one.
float3 hlg_to_nits(float3 e, float peak)
{
    const float a = 0.17883277;
    const float b = 0.28466892;  // 1 - 4a
    const float c = 0.55991073;  // 0.5 - a * ln(4a)
    e = saturate(e);
    float3 scene = e <= 0.5 ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
    // BT.2020's luma coefficients, because HLG's OOTF is defined on BT.2100,
    // whose primaries are BT.2020's.
    float y = dot(scene, float3(0.2627, 0.6780, 0.0593));
    // The system gamma, 1.2 + 0.42 log10(peak / 1000), is the display's and
    // the same for every pixel: `hlg_gamma1` is it less one, from the host.
    return peak * pow(max(y, 1e-6), hlg_gamma1) * scene;
}

/// **BT.2390's EETF**, which is what §9.2 says those bug reports have been
/// asking Microsoft for: a Hermite roll-off applied in the PQ domain, so the
/// knee is where the eye's sensitivity is rather than where the arithmetic is
/// convenient. Black is left at zero -- a lift belongs to a display that cannot
/// reach it, and this one is being asked to reach less light, not more.
///
/// **Normalised to the source's range first, as the recommendation writes
/// it.** The knee sits at 1.5 * maxT - 0.5 with maxT the target's peak as a
/// fraction of the *source's*; a mapper that skipped that step took the source
/// to be PQ's whole 10000 nits, put the knee at about four nits for an 80-nit
/// target, and flattened everything above it: a 1000-nit grade's reference
/// white at two thirds of the display, its midtones lifted towards that and
/// its highlights crushed into the third that was left.
/// `e` is a PQ code value; the peaks' own, their ratio `tone_mt`, the knee
/// `tone_ks` and one over the span above it are the same for every pixel and
/// come worked out from the host.
float eetf_bt2390(float e)
{
    float e1 = e * tone_source_inv;
    float t = saturate((e1 - tone_ks) * tone_span_inv);
    float t2 = t * t;
    float t3 = t2 * t;
    float knee = (2.0 * t3 - 3.0 * t2 + 1.0) * tone_ks +
                 (t3 - 2.0 * t2 + t) * (1.0 - tone_ks) +
                 (-2.0 * t3 + 3.0 * t2) * tone_mt;
    // Nothing above the target, however far past its stated peak the source
    // strays, and nothing rolled off below the knee.
    float mapped = e1 < tone_ks ? e1 : min(knee, tone_mt);
    return mapped * tone_source_pq;
}

/// **On the brightest component, and the other two follow in ratio.** BT.2390
/// offers the EETF per component or on the maximum of the three; per component
/// bends each channel by its own amount and a saturated highlight comes out a
/// different hue and greyer. The ratio form keeps the hue and the saturation
/// and rolls off the brightness, which is the thing a display cannot show.
/// A source the display can show whole is passed through untouched.
float3 tone_map_bt2390(float3 nits)
{
    if (tone_mt >= 1.0) {
        return nits;
    }
    float brightest = max(nits.r, max(nits.g, nits.b));
    if (brightest <= 0.0) {
        return nits;
    }
    float e = nits_to_pq(float3(brightest, brightest, brightest)).x;
    float mapped = pq_to_nits(float3(1.0, 1.0, 1.0) * eetf_bt2390(e)).x;
    return nits * (mapped / brightest);
}

/// The transfer the stream stated, into linear scRGB units where 1.0 is 80
/// nits. **SDR content is scaled and HDR content is not**: §9.6's boost exists
/// because scRGB 1.0 is scene-referred, and PQ already says how many nits it
/// means.
float3 decode(float3 c)
{
    if (transfer_kind == 3) {
        return hlg_to_nits(c, hlg_peak) / 80.0;
    }
    if (transfer_kind == 2) {
        return pq_to_nits(c) / 80.0;
    }
    if (transfer_kind == 4) {
        // Code point 8: linear, which H.273 names and a test pattern uses.
        return saturate(c) * sdr_scale;
    }
    return sdr_to_linear(c) * sdr_scale;
}

// **Chroma, reconstructed at the luma samples' positions, in two halves.**
// Chroma sample k of an axis sits at luma sample k / step + off, so the chroma
// position for luma sample p is (p - off) * step and the kernel is centred
// there. The kernel is separable, so the horizontal half runs once per
// chroma *row* into `chroma_wide` -- the luma's width at the chroma's height
// -- and the main pass takes the vertical taps from that. Edge samples repeat
// past the plane's *picture*, which is the crop and not the texture.
//
// **Two phases per axis, and a run of weights for each.** A subsampled axis
// has one chroma sample per two luma samples, so where the kernel sits
// against the chroma samples is one thing at an even luma sample and another
// at an odd one, and the weights for each are worked out on the host
// (`chroma_weights`). The taps are a loop of fixed length: eight, unrolled,
// which every kernel here fits but a Lanczos of five lobes or more, and that
// takes as many eights as it needs. The reads of eight taps are issued
// together, where a loop as long as the kernel's reach issued them one after
// another.
float2 chroma_sample(int2 c)
{
    return chroma_planar != 0 ? float2(chroma_u.Load(int3(c, 0)).r, chroma_v.Load(int3(c, 0)).r)
                              : chroma.Load(int3(c, 0)).rg;
}

// The horizontal half: `p.x` is a luma column, `p.y` a chroma row.
float2 chroma_across(int2 p)
{
    // One exit: fxc reads an early return ahead of a loop as a path on which
    // the result is never written, and this file compiles warnings as errors.
    float2 result = float2(0.0, 0.0);
    if (chroma_step.x == 1.0) {
        result = chroma_sample(p); // 4:4:4: nothing to reconstruct across
    } else {
        int phase = p.x & 1;
        int first = (p.x >> 1) + chroma_first[phase];
        uint base = uint(phase) * chroma_taps;
        int last = int(chroma_size.x) - 1;
        float2 sum = float2(0.0, 0.0);
        if (chroma_taps == 8) {
            [unroll] for (int t = 0; t < 8; ++t) {
                sum += chroma_weights[base + uint(t)] *
                       chroma_sample(int2(clamp(first + t, 0, last), p.y));
            }
        } else {
            [loop] for (uint g = 0; g < chroma_taps; g += 8) {
                [unroll] for (int u = 0; u < 8; ++u) {
                    int t = int(g) + u;
                    sum += chroma_weights[base + uint(t)] *
                           chroma_sample(int2(clamp(first + t, 0, last), p.y));
                }
            }
        }
        result = sum;
    }
    return result;
}

// The vertical half, at a luma sample, from the rows `chroma_across` made.
float2 chroma_at(int2 p)
{
    float2 result = float2(0.0, 0.0);
    if (chroma_step.y == 1.0) {
        result = chroma_wide.Load(int3(p, 0)).rg; // 4:2:2 and 4:4:4: no vertical siting
    } else {
        int phase = p.y & 1;
        int first = (p.y >> 1) + chroma_first[2 + phase];
        uint base = uint(2 + phase) * chroma_taps;
        int last = int(chroma_size.y) - 1;
        float2 sum = float2(0.0, 0.0);
        if (chroma_taps == 8) {
            [unroll] for (int t = 0; t < 8; ++t) {
                sum += chroma_weights[base + uint(t)] *
                       chroma_wide.Load(int3(p.x, clamp(first + t, 0, last), 0)).rg;
            }
        } else {
            [loop] for (uint g = 0; g < chroma_taps; g += 8) {
                [unroll] for (int u = 0; u < 8; ++u) {
                    int t = int(g) + u;
                    sum += chroma_weights[base + uint(t)] *
                           chroma_wide.Load(int3(p.x, clamp(first + t, 0, last), 0)).rg;
                }
            }
        }
        result = sum;
    }
    return result;
}

// **A colour the display cannot show keeps its brightness and its hue, and
// gives up saturation.** After the gamut matrix a saturated BT.2020 colour has
// a component past 1 or below 0 in BT.709; clipping each on its own is what
// the compositor did, and it changes the hue, throws away the luminance the
// grade put there, and merges every bar of a clipping pattern above the level
// where its own primary runs out -- a different level for each colour. This
// moves the colour towards grey of the same luminance instead, along the line
// through the achromatic axis, which keeps its chromaticity's hue angle, and
// with a soft knee at eighty percent of the way to the boundary so that the
// approach is a gradient rather than a wall. What comes out is inside the
// display's gamut, at the luminance that went in wherever the display has
// it, and the bars stay distinct all the way to the source's peak, as the
// white ones do.
float3 compress_gamut(float3 c)
{
    float y = dot(c, luma709);
    if (y <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    // **At or above the display's white there is only white.** The grey of the
    // same luminance a colour is moved towards does not exist up there, and
    // the way there from below already ends at white: the room above a colour
    // shrinks to nothing as its luminance reaches one, and the compression
    // takes all of its saturation. Clipping each component there instead was
    // a step -- a colour just below the white went to grey, and one at it kept
    // a clipped hue -- which a roll-off that leaves content above its target
    // can reach.
    if (y >= 1.0) {
        return float3(1.0, 1.0, 1.0);
    }
    float3 d = c - y;
    // How far along the line from grey to `c` the gamut's boundary is: at
    // t times the way, some component reaches 1 or 0.
    float t = 1e9;
    [unroll] for (int i = 0; i < 3; ++i) {
        if (d[i] > 1e-9) {
            t = min(t, (1.0 - y) / d[i]);
        } else if (d[i] < -1e-9) {
            t = min(t, -y / d[i]);
        }
    }
    if (t >= 1e8) {
        return c; // achromatic
    }
    float u = 1.0 / t; // the colour's own distance: 1 is on the boundary
    const float knee = 0.8;
    if (u <= knee) {
        return c;
    }
    float compressed = knee + (1.0 - knee) * tanh((u - knee) / (1.0 - knee));
    return y + d * (compressed / u);
}

/// Tone mapping and then the gamut, in that order: the roll-off is defined on
/// the source's own primaries and moving them first would map the wrong
/// luminances.
float3 to_scrgb(float3 c)
{
    float3 light = linear_in != 0 ? c : decode(c);
    if (emit_linear != 0) {
        // Linear, on the source's own primaries: the gamut move belongs with
        // the tone mapping below, and a stage grades what the file is rather
        // than what the display happens to be.
        return light;
    }
    if (emit_pq != 0) {
        // Straight back out as the display would be sent it: no roll-off,
        // because somebody else is about to do that, and no gamut move,
        // because HDR10 *is* BT.2020.
        return nits_to_pq(light * 80.0);
    }
    if (tone_peak > 0.0) {
        // **In units of the target white**, which is the display's: 203 nits
        // of PQ comes out as 1.0 on an SDR display, exactly where an SDR
        // film's white is. With the target at scRGB's 80 the unit and the
        // target agreed with each other and were both the wrong number.
        light = tone_map_bt2390(light * 80.0) * tone_peak_inv;
    }
    float3 moved = float3(dot(gamut0.xyz, light), dot(gamut1.xyz, light),
                          dot(gamut2.xyz, light));
    // **And what the display's gamut cannot hold**, only where a roll-off is
    // in the path: that is an SDR display an HDR picture is being fitted to.
    // On an HDR display scRGB carries BT.2020 as components outside [0, 1]
    // and the compositor knows what they mean.
    if (gamut_mode != 0 && tone_peak > 0.0) {
        moved = compress_gamut(moved);
    }
    return moved;
}

float4 ps_rgba(Vertex input) : SV_Target
{
    float4 texel = rgba.Sample(bilinear, input.uv);
    // **The target is scRGB, which is linear.** An sRGB texture sampled without
    // decoding would be presented as though its gamma were already gone, which
    // is the washed-out picture people mistake for a tone mapping problem.
    return float4(to_scrgb(texel.rgb), texel.a);
}

// **The conversion, given samples that have already been read.** Both pixel
// shaders below reach this with the same three numbers, so there is one matrix
// and one transfer in this file rather than one per plane arrangement -- and a
// 4:2:0 frame and a 4:4:4 frame are held to the same arithmetic by
// construction rather than by two functions being kept in step.
float4 convert(float y, float u, float v)
{
    y = (y - luma_offset) * luma_scale;
    // `has_chroma` is 0 for 4:0:0, where the chroma planes do not exist. It
    // multiplies the *centred* chroma, so grey comes out as grey rather than
    // as whatever an unbound texture samples to.
    u = (u - 0.5) * chroma_scale * has_chroma;
    v = (v - 0.5) * chroma_scale * has_chroma;

    // Non-linear R'G'B' first, which is what the matrix produces, and then the
    // transfer. Doing them the other way round is a mistake that looks almost
    // right and is wrong everywhere the picture is not grey.
    float3 encoded = float3(y + yuv.x * v,
                            y - yuv.y * u - yuv.z * v,
                            y + yuv.w * u);
    return float4(to_scrgb(encoded), 1.0);
}

float4 ps_nv12(Vertex input) : SV_Target
{
    // **Every sample read where it is.** This pass draws at the picture's
    // own size -- the target's when they agree, the source's when the
    // resampler is about to run -- so the pixel being drawn *is* a luma
    // sample and `Load` fetches it exactly, and the chroma is reconstructed
    // at that luma sample's position by the kernel and the siting the
    // constants state. Normalised coordinates through the sampler would have
    // put the crop and the siting in its hands, which is where a quarter-
    // sample error lives undetected.
    int2 p = int2(input.position.xy);
    float y = luma.Load(int3(p, 0)).r * sample_scale;
    float2 uv = has_chroma != 0.0 ? chroma_at(p) * sample_scale : float2(0.5, 0.5);
    return convert(y, uv.x, uv.y);
}

float4 ps_planar(Vertex input) : SV_Target
{
    // Three planes, or one for 4:0:0 -- in which case `has_chroma` is 0 and
    // no chroma is read at all. The two arrangements meet in `chroma_across`,
    // so this and `ps_nv12` are one shader in two names.
    int2 p = int2(input.position.xy);
    float y = luma.Load(int3(p, 0)).r * sample_scale;
    float2 uv = has_chroma != 0.0 ? chroma_at(p) * sample_scale : float2(0.5, 0.5);
    return convert(y, uv.x, uv.y);
}

// **The horizontal half of the chroma reconstruction**, drawn at the luma's
// width and the chroma's height into `chroma_wide` before the main pass.
float4 ps_chroma_across(Vertex input) : SV_Target
{
    return float4(chroma_across(int2(input.position.xy)), 0.0, 1.0);
}


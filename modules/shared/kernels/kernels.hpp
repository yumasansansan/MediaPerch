// SPDX-License-Identifier: GPL-3.0-or-later
//
// The resampling kernels: their names and numbers, their support, and the HLSL
// that computes their weights.
//
// **One formula set, compiled twice.** The scaler stage (`modules/vdsp/scale`)
// resamples the picture and the presenter (`modules/video/d3d11`) reconstructs
// chroma at the luma's positions; both are a weighted sum over taps of a
// kernel centred somewhere between samples, and a kernel that meant one thing
// in the stage and a slightly different thing in the presenter would be a
// difference no test would think to look for. So the names, the numbers the
// shaders switch on, and the shader text itself live here, and each module
// pastes the text into its own shader string before compiling.
//
// **Names and numbers only, no arithmetic in C++.** The weights are computed
// in the shader, once per tap; tests/vdsp_scale_test.cpp holds that shader to
// references written in double from the kernels' published formulas rather
// than to a C++ copy of the same code, which would be the same mistake twice.
//
// **Every parameter is a parameter.** Lanczos takes its lobe count, the cubic
// takes B and C, and nothing here caps either: a thousand-lobe Lanczos is
// thousands of taps per output sample and a person who asks for it gets it,
// and gets told what it costs, which is the tree's rule about its user.

#ifndef MEDIAPERCH_SHARED_KERNELS_HPP
#define MEDIAPERCH_SHARED_KERNELS_HPP

#include <cstdint>
#include <cstring>

namespace mp::kernels {

/// The kernels, numbered as the shader's `kernel_weight` switches on them.
enum class Kernel : std::uint32_t {
    /// The triangle: two taps. What a sampler's bilinear fetch is.
    bilinear = 0,
    /// The box: an area average when downscaling by an integer factor, and
    /// nearest-neighbour when upscaling. What a checkerboard is measured with.
    box = 1,
    /// The cubic Hermite interpolant, 2x^3 - 3x^2 + 1 on [0, 1): the cubic
    /// with B=0, C=0. Two taps, interpolating, **no negative lobes**, so it
    /// cannot ring -- which is why it is the downscaling default here and in
    /// mpv. Slightly soft.
    hermite = 2,
    /// Mitchell and Netravali's two-parameter cubic with `b` and `c` as
    /// given. B=1/3, C=1/3 is their own recommendation; B=0, C=1/2 is
    /// Catmull-Rom; B=1, C=0 is the cubic B-spline.
    bicubic = 3,
    /// Catmull-Rom: the cubic with B=0, C=1/2. Four taps, interpolating.
    catrom = 4,
    /// Mitchell-Netravali, B=C=1/3. Four taps and **not interpolating**: it
    /// blurs a picture even at one to one, by design.
    mitchell = 5,
    /// The cubic splines with 16, 36 and 64 support points, as ImageMagick and
    /// mpv spell them. Less ringing than Lanczos at the same sharpness.
    spline16 = 6,
    spline36 = 7,
    spline64 = 8,
    /// A windowed sinc with `lobes` lobes. Three is the usual choice for
    /// upscaling and the default here; two is softer, four sharper and
    /// ringier, and a thousand is a thousand.
    lanczos = 9,
};

constexpr std::uint32_t k_kernel_count = 10;

/// A kernel and the parameters it takes. The ones it does not take are
/// carried and ignored, so that a person can set `up_b` before `up=bicubic`
/// and have it mean something when they do.
struct KernelParams {
    Kernel kind = Kernel::lanczos;
    std::uint32_t lobes = 3;
    float b = 1.0f / 3.0f;
    float c = 1.0f / 3.0f;
};

/// Half the width of a kernel, in taps at one to one. Stretched by the
/// downscale factor when the picture is being made smaller, so that every
/// source sample under an output sample is counted.
[[nodiscard]] constexpr float support_of(Kernel k, std::uint32_t lobes) noexcept
{
    switch (k) {
    case Kernel::bilinear:
    case Kernel::hermite:
        return 1.0f;
    case Kernel::box:
        return 0.5f;
    case Kernel::bicubic:
    case Kernel::catrom:
    case Kernel::mitchell:
    case Kernel::spline16:
        return 2.0f;
    case Kernel::spline36:
        return 3.0f;
    case Kernel::spline64:
        return 4.0f;
    case Kernel::lanczos:
        return static_cast<float>(lobes);
    }
    return 3.0f;
}

[[nodiscard]] constexpr const char* name_of(Kernel k) noexcept
{
    switch (k) {
    case Kernel::bilinear:
        return "bilinear";
    case Kernel::box:
        return "box";
    case Kernel::hermite:
        return "hermite";
    case Kernel::bicubic:
        return "bicubic";
    case Kernel::catrom:
        return "catrom";
    case Kernel::mitchell:
        return "mitchell";
    case Kernel::spline16:
        return "spline16";
    case Kernel::spline36:
        return "spline36";
    case Kernel::spline64:
        return "spline64";
    case Kernel::lanczos:
        return "lanczos";
    }
    return "lanczos";
}

/// The names, in the shader's order, as a `describe` row's `enum:` list and
/// as the sentence a refusal says.
constexpr const char* k_kernel_list =
    "bilinear,box,hermite,bicubic,catrom,mitchell,spline16,spline36,spline64,lanczos";
constexpr const char* k_kernel_sentence =
    "a kernel is bilinear, box, hermite, bicubic, catrom, mitchell, spline16, spline36, "
    "spline64 or lanczos";

[[nodiscard]] inline bool kernel_from_name(const char* name, Kernel& out) noexcept
{
    if (name == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < k_kernel_count; ++i) {
        const auto k = static_cast<Kernel>(i);
        if (std::strcmp(name, name_of(k)) == 0) {
            out = k;
            return true;
        }
    }
    return false;
}

/// The weights, as HLSL: `kernel_support(kind, lobes)` is half the width in
/// taps at one to one and `kernel_weight(kind, lobes, b, c, x)` the weight at
/// `x` taps from the centre. Pasted into a shader string ahead of whatever
/// calls them. Every function has one exit, because fxc reads an early
/// return ahead of a `[loop]` as a path on which the result is never written
/// and both modules compile warnings as errors.
constexpr char k_kernel_hlsl[] = R"HLSL(
// --------------------------------------------------------------------------
// The kernels (modules/shared/kernels/kernels.hpp)
// --------------------------------------------------------------------------

static const float k_pi = 3.14159265358979;

// Half the width of a kernel, in taps at one to one. `lobes` is Lanczos's.
float kernel_support(uint kind, uint lobes)
{
    float support = 3.0;
    if (kind == 0 || kind == 2) {
        support = 1.0;                // bilinear, hermite
    } else if (kind == 1) {
        support = 0.5;                // box
    } else if (kind == 3 || kind == 4 || kind == 5 || kind == 6) {
        support = 2.0;                // the cubics, spline16
    } else if (kind == 7) {
        support = 3.0;                // spline36
    } else if (kind == 8) {
        support = 4.0;                // spline64
    } else {
        support = float(lobes);       // lanczos
    }
    return support;
}

// Mitchell and Netravali's two-parameter cubic, *Reconstruction Filters in
// Computer Graphics* (1988), equation 8. `x` is non-negative.
float cubic_bc(float b, float c, float x)
{
    float x2 = x * x;
    float x3 = x2 * x;
    float w = 0.0;
    if (x < 1.0) {
        w = ((12.0 - 9.0 * b - 6.0 * c) * x3 + (-18.0 + 12.0 * b + 6.0 * c) * x2 +
             (6.0 - 2.0 * b)) / 6.0;
    } else if (x < 2.0) {
        w = ((-b - 6.0 * c) * x3 + (6.0 * b + 30.0 * c) * x2 + (-12.0 * b - 48.0 * c) * x +
             (8.0 * b + 24.0 * c)) / 6.0;
    }
    return w;
}

// The kernel's weight at `x` taps from its centre. The interpolating ones are
// 1 at 0 and 0 at every other integer, so one to one is the identity;
// Mitchell-Netravali is not, by design.
float kernel_weight(uint kind, uint lobes, float b, float c, float x)
{
    x = abs(x);
    float w = 0.0;
    if (kind == 0) {
        w = max(1.0 - x, 0.0);
    } else if (kind == 1) {
        // The box, with a sample on the boundary shared: an integer downscale
        // then counts every source sample exactly once.
        w = x < 0.5 ? 1.0 : (x == 0.5 ? 0.5 : 0.0);
    } else if (kind == 2) {
        w = x < 1.0 ? (2.0 * x - 3.0) * x * x + 1.0 : 0.0;
    } else if (kind == 3) {
        w = cubic_bc(b, c, x);
    } else if (kind == 4) {
        w = cubic_bc(0.0, 0.5, x);
    } else if (kind == 5) {
        w = cubic_bc(1.0 / 3.0, 1.0 / 3.0, x);
    } else if (kind == 6) {
        if (x < 1.0) {
            w = ((x - 9.0 / 5.0) * x - 1.0 / 5.0) * x + 1.0;
        } else if (x < 2.0) {
            float t = x - 1.0;
            w = ((-1.0 / 3.0 * t + 4.0 / 5.0) * t - 7.0 / 15.0) * t;
        }
    } else if (kind == 7) {
        if (x < 1.0) {
            w = ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
        } else if (x < 2.0) {
            float t = x - 1.0;
            w = ((-6.0 / 11.0 * t + 270.0 / 209.0) * t - 156.0 / 209.0) * t;
        } else if (x < 3.0) {
            float t = x - 2.0;
            w = ((1.0 / 11.0 * t - 45.0 / 209.0) * t + 26.0 / 209.0) * t;
        }
    } else if (kind == 8) {
        if (x < 1.0) {
            w = ((49.0 / 41.0 * x - 6387.0 / 2911.0) * x - 3.0 / 2911.0) * x + 1.0;
        } else if (x < 2.0) {
            float t = x - 1.0;
            w = ((-24.0 / 41.0 * t + 4032.0 / 2911.0) * t - 2328.0 / 2911.0) * t;
        } else if (x < 3.0) {
            float t = x - 2.0;
            w = ((6.0 / 41.0 * t - 1008.0 / 2911.0) * t + 582.0 / 2911.0) * t;
        } else if (x < 4.0) {
            float t = x - 3.0;
            w = ((-1.0 / 41.0 * t + 168.0 / 2911.0) * t - 97.0 / 2911.0) * t;
        }
    } else {
        // Lanczos: sinc(x) * sinc(x / a) inside a lobes. Snapped at the
        // integers, where sin(pi) in single precision is a hundred-millionth
        // rather than nought, so that one to one is the identity to the bit.
        float a = float(lobes);
        if (x < a) {
            float n = round(x);
            if (abs(x - n) < 1e-6) {
                w = n == 0.0 ? 1.0 : 0.0;
            } else {
                float px = k_pi * x;
                w = a * sin(px) * sin(px / a) / (px * px);
            }
        }
    }
    return w;
}
)HLSL";

} // namespace mp::kernels

#endif // MEDIAPERCH_SHARED_KERNELS_HPP

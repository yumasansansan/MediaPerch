// SPDX-License-Identifier: GPL-3.0-or-later
//
// The resampling kernels: their names and numbers, their support, and their
// weights, worked out in double for the shaders to read.
//
// **One formula set, for two modules.** The scaler stage
// (`modules/vdsp/scale`) resamples the picture and the presenter
// (`modules/video/d3d11`) reconstructs chroma at the luma's positions; both
// are a weighted sum over taps of a kernel centred somewhere between samples,
// and a kernel that meant one thing in the stage and a slightly different
// thing in the presenter would be a difference no test would think to look
// for. So the names, the numbers and the arithmetic live here, and both
// modules take their weights from `taps_at`.
//
// **The weights are worked out once, in double, and the shaders read them.**
// A pass reconstructing chroma sees two phases per axis, and a pass of the
// scaler one set of weights per output column or row; the shaders evaluated
// the kernel at every tap of every pixel instead, in single precision, in a
// loop as long as the kernel's reach, which is known only when the shader
// runs. Read from a table, the taps are a loop of fixed length that the
// compiler unrolls and that a GPU issues the reads of together. On an Iris
// Xe the presenter's two chroma passes over a 4K 4:2:0 frame went from 9.5 to
// 5.2 ms (11.4 to 6.1 ms with PQ rolled off), and the scaler's two passes
// through Lanczos 3 from 11.2 to 8.8 ms down to 1080p and from 12.0 to 8.1 ms
// up from it; a loop of the same fixed length with the kernel still
// evaluated in it was slower than the loop it replaced, so it is the table
// that pays. **And the arithmetic is double's.** The positions, the weights
// and their normalisation are worked out in double and rounded to single
// once (`round_run`), where the shaders worked a position out in single --
// whose steps at a 4K picture's coordinates are a quarter of a thousandth of
// a sample -- and a weight from it, and summed and divided in single: the
// scaler's output was up to 1.1e-4 from the same resampling in double, and
// is 1.9e-7.
// tests/vdsp_scale_test.cpp holds the passes to references written in double
// from the kernels' published formulas rather than to a copy of this code,
// which would be the same mistake twice.
//
// **Every parameter is a parameter.** Lanczos takes its lobe count, the cubic
// takes B and C, and nothing here caps either: a thousand-lobe Lanczos is
// thousands of taps per output sample and a person who asks for it gets it,
// and gets told what it costs, which is the tree's rule about its user.

#ifndef MEDIAPERCH_SHARED_KERNELS_HPP
#define MEDIAPERCH_SHARED_KERNELS_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace mp::kernels {

/// The kernels, numbered in the order a row's `enum:` list names them.
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
/// and have it mean something when they do. B and C in double, as they were
/// typed: the weights are worked out from them in double.
struct KernelParams {
    Kernel kind = Kernel::lanczos;
    std::uint32_t lobes = 3;
    double b = 1.0 / 3.0;
    double c = 1.0 / 3.0;
};

/// Half the width of a kernel, in taps at one to one. Stretched by the
/// downscale factor when the picture is being made smaller, so that every
/// source sample under an output sample is counted. In double, which holds
/// every lobe count there is exactly.
[[nodiscard]] constexpr double support_of(Kernel k, std::uint32_t lobes) noexcept
{
    switch (k) {
    case Kernel::bilinear:
    case Kernel::hermite:
        return 1.0;
    case Kernel::box:
        return 0.5;
    case Kernel::bicubic:
    case Kernel::catrom:
    case Kernel::mitchell:
    case Kernel::spline16:
        return 2.0;
    case Kernel::spline36:
        return 3.0;
    case Kernel::spline64:
        return 4.0;
    case Kernel::lanczos:
        return static_cast<double>(lobes);
    }
    return 3.0;
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

/// Mitchell and Netravali's two-parameter cubic, *Reconstruction Filters in
/// Computer Graphics* (1988), equation 8. `x` is non-negative.
[[nodiscard]] inline double cubic_bc(double b, double c, double x) noexcept
{
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (x < 1.0) {
        return (((12.0 - (9.0 * b) - (6.0 * c)) * x3) + ((-18.0 + (12.0 * b) + (6.0 * c)) * x2) +
                (6.0 - (2.0 * b))) /
               6.0;
    }
    if (x < 2.0) {
        return (((-b - (6.0 * c)) * x3) + (((6.0 * b) + (30.0 * c)) * x2) +
                (((-12.0 * b) - (48.0 * c)) * x) + ((8.0 * b) + (24.0 * c))) /
               6.0;
    }
    return 0.0;
}

/// The kernel's weight at `x` taps from its centre. The interpolating ones are
/// 1 at 0 and 0 at every other integer, so one to one is the identity;
/// Mitchell-Netravali is not, by design.
[[nodiscard]] inline double weight(Kernel k, std::uint32_t lobes, double b, double c,
                                   double x) noexcept
{
    x = std::fabs(x);
    switch (k) {
    case Kernel::bilinear:
        return x < 1.0 ? 1.0 - x : 0.0;
    case Kernel::box:
        // The box, with a sample on the boundary shared: an integer downscale
        // then counts every source sample exactly once.
        return x < 0.5 ? 1.0 : (x > 0.5 ? 0.0 : 0.5);
    case Kernel::hermite:
        return x < 1.0 ? ((((2.0 * x) - 3.0) * x) * x) + 1.0 : 0.0;
    case Kernel::bicubic:
        return cubic_bc(b, c, x);
    case Kernel::catrom:
        return cubic_bc(0.0, 0.5, x);
    case Kernel::mitchell:
        return cubic_bc(1.0 / 3.0, 1.0 / 3.0, x);
    case Kernel::spline16:
        if (x < 1.0) {
            return ((((x - (9.0 / 5.0)) * x) - (1.0 / 5.0)) * x) + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return (((((-1.0 / 3.0) * t) + (4.0 / 5.0)) * t) - (7.0 / 15.0)) * t;
        }
        return 0.0;
    case Kernel::spline36:
        if (x < 1.0) {
            return (((((13.0 / 11.0) * x) - (453.0 / 209.0)) * x) - (3.0 / 209.0)) * x + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return (((((-6.0 / 11.0) * t) + (270.0 / 209.0)) * t) - (156.0 / 209.0)) * t;
        }
        if (x < 3.0) {
            const double t = x - 2.0;
            return (((((1.0 / 11.0) * t) - (45.0 / 209.0)) * t) + (26.0 / 209.0)) * t;
        }
        return 0.0;
    case Kernel::spline64:
        if (x < 1.0) {
            return (((((49.0 / 41.0) * x) - (6387.0 / 2911.0)) * x) - (3.0 / 2911.0)) * x + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return (((((-24.0 / 41.0) * t) + (4032.0 / 2911.0)) * t) - (2328.0 / 2911.0)) * t;
        }
        if (x < 3.0) {
            const double t = x - 2.0;
            return (((((6.0 / 41.0) * t) - (1008.0 / 2911.0)) * t) + (582.0 / 2911.0)) * t;
        }
        if (x < 4.0) {
            const double t = x - 3.0;
            return (((((-1.0 / 41.0) * t) + (168.0 / 2911.0)) * t) - (97.0 / 2911.0)) * t;
        }
        return 0.0;
    case Kernel::lanczos: {
        // sinc(x) * sinc(x / a) inside a lobes. Snapped at the integers, where
        // sin(pi n) is a rounding rather than nought, so that one to one is
        // the identity to the bit.
        const auto a = static_cast<double>(lobes);
        if (!(x < a)) {
            return 0.0;
        }
        const double n = std::round(x);
        if (std::fabs(x - n) < 1e-12) {
            return n < 0.5 ? 1.0 : 0.0;
        }
        const double px = std::numbers::pi * x;
        return a * std::sin(px) * std::sin(px / a) / (px * px);
    }
    }
    return 0.0;
}

/// The taps a kernel reaches centred at `centre`, in samples, and stretched
/// `stretch` times -- 1 when growing, the factor when shrinking, so that every
/// sample under an output sample is counted: the first sample it reaches and
/// every weight from there, normalised to add up to one. Weights that add up
/// to nothing are left as they are rather than divided by that.
struct Taps {
    std::int64_t first = 0;
    std::vector<double> weights;
};

[[nodiscard]] inline Taps taps_at(const KernelParams& k, double centre, double stretch)
{
    const double reach = support_of(k.kind, k.lobes) * stretch;
    const auto lo = static_cast<std::int64_t>(std::ceil(centre - reach));
    const auto hi = static_cast<std::int64_t>(std::floor(centre + reach));
    Taps out;
    out.first = lo;
    double sum = 0.0;
    for (std::int64_t j = lo; j <= hi; ++j) {
        const double w =
            weight(k.kind, k.lobes, k.b, k.c, (centre - static_cast<double>(j)) / stretch);
        out.weights.push_back(w);
        sum += w;
    }
    if (std::fabs(sum) >= 1e-6) {
        for (double& w : out.weights) {
            w /= sum;
        }
    }
    return out;
}

/// **A run of weights rounded to single precision for a shader, adding up to
/// what it added up to.** Each weight rounded on its own leaves the run's sum
/// off by as much as half a unit in the last place for every tap, which on a
/// flat field is a gain that is not one; what the rounding took from the sum
/// is given back to the largest weight, which takes it with the least change
/// relative to its size.
inline void round_run(const std::vector<double>& weights, float* out) noexcept
{
    if (weights.empty()) {
        return;
    }
    double exact = 0.0;
    double rounded = 0.0;
    std::size_t largest = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        out[i] = static_cast<float>(weights[i]);
        exact += weights[i];
        rounded += static_cast<double>(out[i]);
        if (std::fabs(weights[i]) > std::fabs(weights[largest])) {
            largest = i;
        }
    }
    out[largest] = static_cast<float>(static_cast<double>(out[largest]) + (exact - rounded));
}

} // namespace mp::kernels

#endif // MEDIAPERCH_SHARED_KERNELS_HPP

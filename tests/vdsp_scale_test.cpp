// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scaler stage, held to references written from the kernels' published
// formulas rather than to the shader's own arithmetic.
//
// **What the HDR10 test patterns found, the second time.** The presenter's
// own resampler (§9.11) fixed the moiré and the vanishing rulers and then
// showed a fault of its own: Lanczos, three lobes, stretched over a one-pixel
// ruler on a grey field, rings -- a dark halo either side of every line,
// wider than the line. The fix is a kernel with no negative lobes for going
// down, an antiringing clamp for the ones that have them, and every parameter
// of every kernel in a person's hands; and all of that is a stage, so that
// there is a second implementation to hold the presenter's fetch to and a
// node a person can take out. The properties:
//
//   - with no size asked of it the stage is the identity and draws nothing;
//   - it answers the size the presenter asked for, and the picture comes back
//     at that size, through the presenter's own finish pass;
//   - a two-to-one checkerboard is a flat grey, in linear light, for every
//     kernel: what a downscale *is*;
//   - the real shader resamples as a double-precision reference does, up,
//     down and one axis at a time, for every kernel and for a thousand lobes;
//   - a one-pixel ruler on a field does not ring through hermite, does through
//     lanczos, and does not once antiring is on.
//
// Everything renders off screen on WARP and reads back fp32, the way
// scaler_test.cpp and vdsp_lut_test.cpp do.

#include <mediaperch/module.h>

#include "module_loader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

using Catch::Approx;

namespace {

using mp::test::Module;

// --------------------------------------------------------------------------
// The reference: the kernels as the literature states them
// --------------------------------------------------------------------------

enum class Kind {
    bilinear,
    box,
    hermite,
    bicubic,
    catrom,
    mitchell,
    spline16,
    spline36,
    spline64,
    lanczos
};

constexpr std::array<Kind, 10> k_kinds{Kind::bilinear, Kind::box,      Kind::hermite,
                                       Kind::bicubic,  Kind::catrom,   Kind::mitchell,
                                       Kind::spline16, Kind::spline36, Kind::spline64,
                                       Kind::lanczos};

struct Params {
    Kind kind = Kind::lanczos;
    int lobes = 3;
    double b = 1.0 / 3.0;
    double c = 1.0 / 3.0;
};

const char* name_of(Kind k)
{
    switch (k) {
    case Kind::bilinear:
        return "bilinear";
    case Kind::box:
        return "box";
    case Kind::hermite:
        return "hermite";
    case Kind::bicubic:
        return "bicubic";
    case Kind::catrom:
        return "catrom";
    case Kind::mitchell:
        return "mitchell";
    case Kind::spline16:
        return "spline16";
    case Kind::spline36:
        return "spline36";
    case Kind::spline64:
        return "spline64";
    case Kind::lanczos:
        return "lanczos";
    }
    return "?";
}

double support_of(const Params& p)
{
    switch (p.kind) {
    case Kind::bilinear:
    case Kind::hermite:
        return 1.0;
    case Kind::box:
        return 0.5;
    case Kind::bicubic:
    case Kind::catrom:
    case Kind::mitchell:
    case Kind::spline16:
        return 2.0;
    case Kind::spline36:
        return 3.0;
    case Kind::spline64:
        return 4.0;
    case Kind::lanczos:
        return p.lobes;
    }
    return 3.0;
}

/// Mitchell and Netravali's cubic, *Reconstruction Filters in Computer
/// Graphics* (1988), equation 8.
double cubic_bc(double b, double c, double x)
{
    x = std::fabs(x);
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x3 + (-18.0 + 12.0 * b + 6.0 * c) * x2 +
                (6.0 - 2.0 * b)) /
               6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x3 + (6.0 * b + 30.0 * c) * x2 + (-12.0 * b - 48.0 * c) * x +
                (8.0 * b + 24.0 * c)) /
               6.0;
    }
    return 0.0;
}

double weight(const Params& p, double x)
{
    x = std::fabs(x);
    switch (p.kind) {
    case Kind::bilinear:
        return std::max(1.0 - x, 0.0);
    case Kind::box:
        return x < 0.5 ? 1.0 : (x == 0.5 ? 0.5 : 0.0);
    case Kind::hermite:
        return x < 1.0 ? (2.0 * x - 3.0) * x * x + 1.0 : 0.0;
    case Kind::bicubic:
        return cubic_bc(p.b, p.c, x);
    case Kind::catrom:
        return cubic_bc(0.0, 0.5, x);
    case Kind::mitchell:
        return cubic_bc(1.0 / 3.0, 1.0 / 3.0, x);
    case Kind::spline16:
        if (x < 1.0) {
            return ((x - 9.0 / 5.0) * x - 1.0 / 5.0) * x + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return ((-1.0 / 3.0 * t + 4.0 / 5.0) * t - 7.0 / 15.0) * t;
        }
        return 0.0;
    case Kind::spline36:
        if (x < 1.0) {
            return ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return ((-6.0 / 11.0 * t + 270.0 / 209.0) * t - 156.0 / 209.0) * t;
        }
        if (x < 3.0) {
            const double t = x - 2.0;
            return ((1.0 / 11.0 * t - 45.0 / 209.0) * t + 26.0 / 209.0) * t;
        }
        return 0.0;
    case Kind::spline64:
        if (x < 1.0) {
            return ((49.0 / 41.0 * x - 6387.0 / 2911.0) * x - 3.0 / 2911.0) * x + 1.0;
        }
        if (x < 2.0) {
            const double t = x - 1.0;
            return ((-24.0 / 41.0 * t + 4032.0 / 2911.0) * t - 2328.0 / 2911.0) * t;
        }
        if (x < 3.0) {
            const double t = x - 2.0;
            return ((6.0 / 41.0 * t - 1008.0 / 2911.0) * t + 582.0 / 2911.0) * t;
        }
        if (x < 4.0) {
            const double t = x - 3.0;
            return ((-1.0 / 41.0 * t + 168.0 / 2911.0) * t - 97.0 / 2911.0) * t;
        }
        return 0.0;
    case Kind::lanczos: {
        const double a = p.lobes;
        if (x >= a) {
            return 0.0;
        }
        if (x < 1e-12) {
            return 1.0;
        }
        const double px = 3.14159265358979323846 * x;
        return a * std::sin(px) * std::sin(px / a) / (px * px);
    }
    }
    return 0.0;
}

/// One axis: `src` samples to `dst`, by the convention the stage states.
/// Output i sits at (i + 0.5) * src / dst - 0.5 in source samples; the kernel
/// is stretched by the downscale factor, the edge is clamped, and the weights
/// are normalised. One to one is left alone, as the stage leaves it.
std::vector<double> resample_axis(const std::vector<double>& in, std::size_t dst,
                                  const Params& up, const Params& down)
{
    const std::size_t src = in.size();
    if (src == dst) {
        return in;
    }
    const Params& p = dst < src ? down : up;
    const double ratio = static_cast<double>(src) / static_cast<double>(dst);
    const double stretch = std::max(ratio, 1.0);
    const double reach = support_of(p) * stretch;
    std::vector<double> out(dst);
    for (std::size_t i = 0; i < dst; ++i) {
        const double s = (static_cast<double>(i) + 0.5) * ratio - 0.5;
        const auto lo = static_cast<long>(std::ceil(s - reach));
        const auto hi = static_cast<long>(std::floor(s + reach));
        double sum = 0.0;
        double weights = 0.0;
        for (long j = lo; j <= hi; ++j) {
            const double w = weight(p, (s - static_cast<double>(j)) / stretch);
            if (w == 0.0) {
                continue;
            }
            const long jc = std::clamp(j, 0L, static_cast<long>(src) - 1L);
            sum += w * in[static_cast<std::size_t>(jc)];
            weights += w;
        }
        out[i] = sum / weights;
    }
    return out;
}

/// Both axes, horizontally first, as the stage does it.
std::vector<double> resample(const std::vector<double>& in, std::size_t w, std::size_t h,
                             std::size_t dw, std::size_t dh, const Params& up,
                             const Params& down)
{
    std::vector<double> rows(dw * h);
    for (std::size_t y = 0; y < h; ++y) {
        const std::vector<double> row(in.begin() + static_cast<std::ptrdiff_t>(y * w),
                                      in.begin() + static_cast<std::ptrdiff_t>((y + 1) * w));
        const std::vector<double> scaled = resample_axis(row, dw, up, down);
        std::copy(scaled.begin(), scaled.end(),
                  rows.begin() + static_cast<std::ptrdiff_t>(y * dw));
    }
    std::vector<double> out(dw * dh);
    for (std::size_t x = 0; x < dw; ++x) {
        std::vector<double> column(h);
        for (std::size_t y = 0; y < h; ++y) {
            column[y] = rows[y * dw + x];
        }
        const std::vector<double> scaled = resample_axis(column, dh, up, down);
        for (std::size_t y = 0; y < dh; ++y) {
            out[y * dw + x] = scaled[y];
        }
    }
    return out;
}

// --------------------------------------------------------------------------
// An off-screen presenter, and a stage opened on its device
// --------------------------------------------------------------------------

class Canvas {
public:
    explicit Canvas(const MpVideoVtbl& vtbl) : vtbl_(&vtbl)
    {
        if (vtbl_->open(nullptr, &handle_) != MP_OK) {
            handle_ = nullptr;
            return;
        }
        // WARP, so the pixels are the same on every machine; and an SDR
        // display stated rather than probed, so `sdr_scale` is 1.
        ok_ = vtbl_->set(handle_, "device", "warp") == MP_OK &&
              vtbl_->set(handle_, "display", "hdr=0,white=80,peak=400") == MP_OK;
    }
    ~Canvas()
    {
        if (handle_ != nullptr) {
            vtbl_->close(handle_);
        }
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr && ok_; }
    [[nodiscard]] MpVideo* handle() const noexcept { return handle_; }
    [[nodiscard]] const MpVideoVtbl& vtbl() const noexcept { return *vtbl_; }

    [[nodiscard]] MpResult set(const char* key, const char* value)
    {
        return vtbl_->set(handle_, key, value);
    }
    [[nodiscard]] MpResult configure(const MpVideoInfo& info)
    {
        return vtbl_->configure(handle_, &info);
    }
    [[nodiscard]] MpResult present(const MpVideoFrame& frame)
    {
        return vtbl_->present(handle_, &frame);
    }
    [[nodiscard]] bool device(MpGraphicsDevice& out)
    {
        out = MpGraphicsDevice{};
        out.size = sizeof(out);
        return vtbl_->get_device(handle_, &out) == MP_OK;
    }

    [[nodiscard]] std::string described(const char* key) const
    {
        for (std::uint32_t i = 0;; ++i) {
            char row[512];
            if (vtbl_->describe(handle_, i, row, sizeof row) != MP_OK) {
                return {};
            }
            const std::string line{row};
            const std::size_t first = line.find('\t');
            if (first == std::string::npos || line.substr(0, first) != key) {
                continue;
            }
            const std::size_t second = line.find('\t', first + 1);
            return line.substr(first + 1, second == std::string::npos
                                              ? std::string::npos
                                              : second - first - 1);
        }
    }

    /// Every pixel, fp32 RGBA, and the size it came back at.
    [[nodiscard]] std::vector<float> pixels(std::uint32_t& width, std::uint32_t& height)
    {
        MpPixelLayout layout{};
        layout.size = sizeof(layout);
        if (vtbl_->read_back(handle_, nullptr, 0, &width, &height, &layout) !=
                MP_ERR_NO_MEMORY ||
            (layout.flags & MP_PIXEL_FLOAT) == 0u || layout.container_bits != 32u) {
            return {};
        }
        std::vector<float> out(static_cast<std::size_t>(width) * height * 4u);
        if (vtbl_->read_back(handle_, out.data(), out.size() * sizeof(float), &width, &height,
                             &layout) != MP_OK) {
            return {};
        }
        return out;
    }

private:
    const MpVideoVtbl* vtbl_;
    MpVideo* handle_ = nullptr;
    bool ok_ = false;
};

/// One scaler stage, opened on the canvas's device -- which exists once the
/// canvas is configured, and not before.
class Stage {
public:
    Stage(const MpVideoDspVtbl& vtbl, Canvas& canvas) : vtbl_(&vtbl)
    {
        MpGraphicsDevice device{};
        if (!canvas.device(device)) {
            return;
        }
        if (vtbl_->open(&device, &handle_) != MP_OK) {
            handle_ = nullptr;
        }
    }
    ~Stage()
    {
        if (handle_ != nullptr) {
            vtbl_->close(handle_);
        }
    }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] MpVideoDsp* handle() const noexcept { return handle_; }
    [[nodiscard]] const MpVideoDspVtbl& vtbl() const noexcept { return *vtbl_; }
    [[nodiscard]] MpResult set(const char* key, const char* value)
    {
        return vtbl_->set(handle_, key, value);
    }
    [[nodiscard]] MpVideoStage handed() const noexcept
    {
        MpVideoStage one{};
        one.size = sizeof(one);
        one.vtbl = vtbl_;
        one.handle = handle_;
        return one;
    }
    /// The one stage, handed to the canvas as its whole chain.
    [[nodiscard]] bool attach(Canvas& canvas) const
    {
        const MpVideoStage one = handed();
        return canvas.vtbl().stages(canvas.handle(), &one, 1) == MP_OK;
    }
    [[nodiscard]] std::string described(const char* key) const
    {
        for (std::uint32_t i = 0;; ++i) {
            char row[512];
            if (vtbl_->describe(handle_, i, row, sizeof row) != MP_OK) {
                return {};
            }
            const std::string line{row};
            const std::size_t first = line.find('\t');
            if (first == std::string::npos || line.substr(0, first) != key) {
                continue;
            }
            const std::size_t second = line.find('\t', first + 1);
            return line.substr(first + 1, second == std::string::npos
                                              ? std::string::npos
                                              : second - first - 1);
        }
    }

private:
    const MpVideoDspVtbl* vtbl_;
    MpVideoDsp* handle_ = nullptr;
};

/// A monochrome, full-range, *linear* stream: code point 8, so that what is
/// read back is the sample itself and the resampling is the only arithmetic
/// between the two.
MpVideoInfo linear_mono(std::uint32_t width, std::uint32_t height)
{
    MpVideoInfo info{};
    info.size = sizeof(info);
    info.width = width;
    info.height = height;
    info.display_width = width;
    info.display_height = height;
    info.primaries = 1;
    info.transfer = 8; // linear
    info.matrix = 1;
    info.flags = MP_VIDEO_FULL_RANGE;
    info.timescale = 24000;
    return info;
}

MpVideoFrame mono16(const std::vector<std::uint16_t>& plane, std::uint32_t width,
                    std::uint32_t height)
{
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout.size = sizeof(frame.layout);
    frame.layout.chroma = MP_CHROMA_MONO;
    frame.layout.packing = MP_PACK_PLANAR;
    frame.layout.bits = 16;
    frame.layout.container_bits = 16;
    frame.layout.shift = 0;
    frame.width = width;
    frame.height = height;
    frame.plane[0] = plane.data();
    frame.stride[0] = width * 2u;
    return frame;
}

std::vector<std::uint16_t> random_plane(std::uint32_t width, std::uint32_t height,
                                        std::uint32_t seed)
{
    std::mt19937 twister{seed};
    std::uniform_int_distribution<std::uint32_t> code{0u, 65535u};
    std::vector<std::uint16_t> plane(static_cast<std::size_t>(width) * height);
    for (auto& sample : plane) {
        sample = static_cast<std::uint16_t>(code(twister));
    }
    return plane;
}

std::string size_string(std::uint32_t width, std::uint32_t height)
{
    return std::to_string(width) + "x" + std::to_string(height);
}

/// The picture at one to one, as the presenter decodes it: what the reference
/// resamples, so that the range mapping is not part of the comparison.
std::vector<double> rendered_linear(Canvas& canvas, const std::vector<std::uint16_t>& plane,
                                    std::uint32_t width, std::uint32_t height)
{
    REQUIRE(canvas.set("size", "native") == MP_OK);
    REQUIRE(canvas.configure(linear_mono(width, height)) == MP_OK);
    REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const std::vector<float> got = canvas.pixels(w, h);
    REQUIRE(w == width);
    REQUIRE(h == height);
    REQUIRE(got.size() == static_cast<std::size_t>(width) * height * 4u);
    std::vector<double> out(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = got[i * 4u];
    }
    return out;
}

/// The red channel of every pixel, and the size.
std::vector<double> red_of(Canvas& canvas, std::uint32_t want_width, std::uint32_t want_height)
{
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const std::vector<float> got = canvas.pixels(w, h);
    REQUIRE(w == want_width);
    REQUIRE(h == want_height);
    REQUIRE(got.size() == static_cast<std::size_t>(w) * h * 4u);
    std::vector<double> out(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = got[i * 4u];
        // Grey in, grey out: the three channels are one number.
        CHECK(got[i * 4u + 1u] == Approx(got[i * 4u]).margin(1e-6));
        CHECK(got[i * 4u + 2u] == Approx(got[i * 4u]).margin(1e-6));
    }
    return out;
}

} // namespace

TEST_CASE("the kernels are what the literature says they are", "[vdsp][scale]")
{
    // Interpolating: 1 at the centre and 0 at every other integer, so that one
    // to one is the identity. Mitchell-Netravali is not, and says so in its
    // own paper: it trades that for the absence of ringing.
    for (const Kind k : {Kind::bilinear, Kind::hermite, Kind::catrom, Kind::spline16,
                         Kind::spline36, Kind::spline64, Kind::lanczos}) {
        INFO(name_of(k));
        const Params p{k, 3, 0.0, 0.5};
        CHECK(weight(p, 0.0) == Approx(1.0));
        for (int n = 1; n <= 4; ++n) {
            CHECK(weight(p, n) == Approx(0.0).margin(1e-12));
            CHECK(weight(p, -n) == Approx(0.0).margin(1e-12));
        }
    }
    // And Lanczos at every lobe count, including one somebody typed.
    for (const int lobes : {1, 2, 3, 4, 1000}) {
        const Params p{Kind::lanczos, lobes, 0.0, 0.0};
        INFO("lanczos " << lobes);
        CHECK(weight(p, 0.0) == Approx(1.0));
        CHECK(weight(p, 1.0) == Approx(0.0).margin(1e-12));
        CHECK(weight(p, lobes - 0.5) != 0.0);
        CHECK(weight(p, lobes) == 0.0);
        CHECK(support_of(p) == lobes);
    }
    CHECK(weight(Params{Kind::mitchell}, 0.0) == Approx(8.0 / 9.0));
    CHECK(weight(Params{Kind::mitchell}, 1.0) == Approx(1.0 / 18.0));
    // The parameterised cubic is the named ones at their parameters.
    for (double x = 0.0; x < 2.5; x += 0.13) {
        CHECK(weight(Params{Kind::bicubic, 3, 0.0, 0.5}, x) == Approx(weight(Params{Kind::catrom}, x)));
        CHECK(weight(Params{Kind::bicubic, 3, 1.0 / 3.0, 1.0 / 3.0}, x) ==
              Approx(weight(Params{Kind::mitchell}, x)));
        CHECK(weight(Params{Kind::bicubic, 3, 0.0, 0.0}, x) == Approx(weight(Params{Kind::hermite}, x)));
    }
    CHECK(weight(Params{Kind::box}, 0.0) == 1.0);
    CHECK(weight(Params{Kind::box}, 0.5) == 0.5);
    CHECK(weight(Params{Kind::box}, 0.6) == 0.0);
    // Hermite and the box are never negative, which is the whole reason one
    // of them is the downscaling default.
    for (double x = -1.5; x < 1.5; x += 0.01) {
        CHECK(weight(Params{Kind::hermite}, x) >= 0.0);
        CHECK(weight(Params{Kind::box}, x) >= 0.0);
    }
    // Symmetric, every one of them, and zero past their support.
    for (const Kind k : k_kinds) {
        INFO(name_of(k));
        const Params p{k};
        for (double x = 0.0; x < 4.5; x += 0.37) {
            CHECK(weight(p, x) == Approx(weight(p, -x)));
        }
        CHECK(weight(p, support_of(p) + 0.01) == 0.0);
    }
}

TEST_CASE("with no size asked of it, the stage is the identity and draws nothing",
          "[vdsp][scale]")
{
    // **Zeros ask for the identity.** A host that pre-fills nothing gets the
    // picture back as it is: `configure` answers the input's size and
    // `process` hands the frame back untouched, the same texture, no draw --
    // so a scaler in a chain at one to one costs a pointer copy.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());
    REQUIRE(canvas.set("size", "native") == MP_OK);
    REQUIRE(canvas.configure(linear_mono(16, 12)) == MP_OK);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());

    MpVideoInfo in = linear_mono(16, 12);
    MpVideoInfo answered{};
    answered.size = sizeof(answered);
    REQUIRE(stage.vtbl().configure(stage.handle(), &in, &answered) == MP_OK);
    CHECK(answered.width == 16u);
    CHECK(answered.height == 12u);
    CHECK(stage.described("scale") == "16x12 1:1");

    // A texture of the picture's size on the same device, handed in as a
    // frame the way the presenter hands its intermediate.
    MpGraphicsDevice graphics{};
    REQUIRE(canvas.device(graphics));
    auto* device = static_cast<ID3D11Device*>(graphics.device);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16;
    desc.Height = 12;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.width = 16;
    frame.height = 12;
    frame.layout = MP_LAYOUT_RGBA32F;
    frame.texture = texture;
    MpVideoFrame out{};
    out.size = sizeof(out);
    REQUIRE(stage.vtbl().process(stage.handle(), &frame, &out) == MP_OK);
    CHECK(out.texture == texture);
    CHECK(out.width == 16u);
    CHECK(out.height == 12u);
    CHECK(stage.described("cost") == "0 taps across, 0 down, 0 passes");

    // Asked for a size, it answers that size and produces its own texture.
    answered = MpVideoInfo{};
    answered.size = sizeof(answered);
    answered.width = 8;
    answered.height = 6;
    REQUIRE(stage.vtbl().configure(stage.handle(), &in, &answered) == MP_OK);
    CHECK(answered.width == 8u);
    CHECK(answered.height == 6u);
    CHECK(answered.display_width == 8u);
    CHECK(answered.transfer == 8u);
    REQUIRE(stage.vtbl().process(stage.handle(), &frame, &out) == MP_OK);
    CHECK(out.texture != nullptr);
    CHECK(out.texture != texture);
    CHECK(out.width == 8u);
    CHECK(out.height == 6u);
    CHECK(stage.described("scale") == "16x12 -> 8x6, x0.500 across, x0.500 down");
    // Hermite down at two to one: support 1, stretched by 2, is four taps.
    CHECK(stage.described("cost") == "4 taps across, 4 down, 2 passes");

    // A frame in system memory is not something this can resample.
    MpVideoFrame planes{};
    planes.size = sizeof(planes);
    planes.width = 16;
    planes.height = 12;
    CHECK(stage.vtbl().process(stage.handle(), &planes, &out) == MP_ERR_UNSUPPORTED);
    texture->Release();
}

TEST_CASE("the stage answers the size the presenter asked for, and the picture comes back at it",
          "[vdsp][scale]")
{
    // **The pre-fill contract, end to end.** The presenter fills `out` with
    // the target's size, the stage answers it, and what `read_back` hands
    // over is that size, drawn by the presenter's finish pass from the
    // stage's texture at one to one. The row says who scaled.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 32;
    const std::uint32_t height = 24;
    constexpr std::uint16_t dark = 10000;
    constexpr std::uint16_t bright = 50000;
    std::vector<std::uint16_t> plane(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            plane[static_cast<std::size_t>(y) * width + x] = ((x + y) & 1u) != 0 ? bright : dark;
        }
    }
    const double mean = (dark + bright) / 2.0 / 65535.0;

    REQUIRE(canvas.set("size", "16x12") == MP_OK);
    REQUIRE(canvas.configure(linear_mono(width, height)) == MP_OK);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());
    REQUIRE(stage.attach(canvas));

    // **What a downscale is.** Every output sample sits on the boundary
    // between two source samples, on both axes, and a symmetric kernel gives
    // the two sides of that boundary equal weight; with the source alternating
    // the sum is the mean whatever the kernel -- in linear light, which is
    // what the eye averages when it looks at a panel from across a room.
    for (const Kind k : k_kinds) {
        INFO(name_of(k));
        REQUIRE(stage.set("down", name_of(k)) == MP_OK);
        REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
        const std::vector<double> got = red_of(canvas, 16, 12);
        // **The interior.** At the edge the kernel's taps past the picture
        // repeat the edge sample, which gives one parity of the checkerboard
        // extra weight there. Three output samples in is past the widest
        // kernel's reach at this factor.
        for (std::uint32_t y = 3; y < 9; ++y) {
            for (std::uint32_t x = 3; x < 13; ++x) {
                CHECK(got[static_cast<std::size_t>(y) * 16u + x] == Approx(mean).margin(2e-5));
            }
        }
    }
    CHECK(canvas.described("scale") == "32x24 -> 16x12, x0.500 by the chain");
    CHECK(stage.described("scale") == "32x24 -> 16x12, x0.500 across, x0.500 down");
    CHECK(canvas.described("chain") == "1 stage, with an intermediate at 32x24");
}

TEST_CASE("the stage resamples as the reference does, up, down and one axis at a time",
          "[vdsp][scale]")
{
    // The reference is double precision from the formulas above; the shader
    // is single precision with its own sin. Two ten-thousandths is the room
    // that leaves, on a random picture, and the sizes are chosen so that no
    // output sample of the box kernel lands exactly on a tie between two
    // source samples, where the two precisions would rightly pick
    // differently: a tie is (2i + 1) * src / (2 * dst) landing on an
    // integer, which 16 to 48 and 12 to 36 never do.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 16;
    const std::uint32_t height = 12;
    const std::vector<std::uint16_t> plane = random_plane(width, height, 11u);
    const std::vector<double> source = rendered_linear(canvas, plane, width, height);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());
    REQUIRE(stage.attach(canvas));

    struct Target {
        std::uint32_t width;
        std::uint32_t height;
        const char* what;
    };
    const Target targets[] = {{48, 36, "up"}, {7, 5, "down"}, {16, 24, "one axis"}};
    for (const Target& target : targets) {
        REQUIRE(canvas.set("size", size_string(target.width, target.height).c_str()) == MP_OK);
        for (const Kind k : k_kinds) {
            INFO(name_of(k) << ", " << target.what << " to " << target.width << "x"
                            << target.height);
            // The same kernel both ways, so the direction is the only
            // difference; the cubic at Catmull-Rom's parameters, so that
            // `bicubic` is exercised with something other than its default.
            REQUIRE(stage.set("up", name_of(k)) == MP_OK);
            REQUIRE(stage.set("down", name_of(k)) == MP_OK);
            REQUIRE(stage.set("up_b", "0") == MP_OK);
            REQUIRE(stage.set("up_c", "0.5") == MP_OK);
            REQUIRE(stage.set("down_b", "0") == MP_OK);
            REQUIRE(stage.set("down_c", "0.5") == MP_OK);
            REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
            const std::vector<double> got = red_of(canvas, target.width, target.height);
            const Params p{k, 3, 0.0, 0.5};
            const std::vector<double> want =
                resample(source, width, height, target.width, target.height, p, p);
            REQUIRE(got.size() == want.size());
            double worst = 0.0;
            for (std::size_t i = 0; i < want.size(); ++i) {
                worst = std::max(worst, std::fabs(got[i] - want[i]));
            }
            CHECK(worst < 2e-4);
        }
    }
    // **Up and down are different kernels**, and the axis that shrinks gets
    // the second: 16x12 to 24x9 grows across and shrinks down.
    REQUIRE(canvas.set("size", "24x9") == MP_OK);
    REQUIRE(stage.set("up", "lanczos") == MP_OK);
    REQUIRE(stage.set("down", "hermite") == MP_OK);
    REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
    const std::vector<double> got = red_of(canvas, 24, 9);
    const std::vector<double> want = resample(source, width, height, 24, 9,
                                              Params{Kind::lanczos, 3}, Params{Kind::hermite});
    double worst = 0.0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        worst = std::max(worst, std::fabs(got[i] - want[i]));
    }
    CHECK(worst < 2e-4);
    CHECK(stage.described("scale") == "16x12 -> 24x9, x1.500 across, x0.750 down");
    // Lanczos-3 up is six taps; hermite down at 4:3 is 2 * ceil(1.333) = 4.
    CHECK(stage.described("cost") == "6 taps across, 4 down, 2 passes");
}

TEST_CASE("a thousand lobes is accepted, said back, and still the reference", "[vdsp][scale]")
{
    // **The tree's rule about its user.** A thousand-lobe Lanczos is a
    // thousand taps either side of every output sample; on an 8 by 8 picture
    // nearly all of them clamp to an edge, which is exactly the case where a
    // shader that capped the loop would quietly differ from a reference that
    // did not. The row says what it costs; nothing refuses it.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 8;
    const std::uint32_t height = 8;
    const std::vector<std::uint16_t> plane = random_plane(width, height, 5u);
    const std::vector<double> source = rendered_linear(canvas, plane, width, height);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());
    REQUIRE(stage.attach(canvas));
    REQUIRE(stage.set("up", "lanczos") == MP_OK);
    REQUIRE(stage.set("up_lobes", "1000") == MP_OK);
    CHECK(stage.described("up_lobes") == "1000");

    REQUIRE(canvas.set("size", "12x12") == MP_OK);
    REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
    const std::vector<double> got = red_of(canvas, 12, 12);
    const Params p{Kind::lanczos, 1000};
    const std::vector<double> want = resample(source, width, height, 12, 12, p, p);
    double worst = 0.0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        worst = std::max(worst, std::fabs(got[i] - want[i]));
    }
    // Wider than the three-lobe margin: a thousand sines of arguments up to
    // three thousand radians, in single precision, each contributing a
    // thousandth of a weight.
    CHECK(worst < 5e-3);
    CHECK(stage.described("cost") == "2000 taps across, 2000 down, 2 passes");
}

TEST_CASE("a one-pixel ruler on a field does not ring through hermite, does through lanczos, "
          "and does not once antiring is on",
          "[vdsp][scale]")
{
    // **The aspect-ratio pattern, in miniature.** A grey field with a white
    // line one pixel in from each edge, downscaled by a factor that is not
    // an integer -- 64 to 30 -- which is what the 3840x1606 pattern in a
    // window is. Lanczos-3's negative lobes, stretched over the field, put a
    // dark halo either side of the line: samples below the field's own
    // value, which a field with nothing darker on it cannot honestly have.
    // Hermite has no negative lobes and cannot. And antiring pulls Lanczos
    // back inside the range of the samples it straddles.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t size = 64;
    constexpr std::uint16_t field = 26214; // 0.4 of full range, exactly
    constexpr std::uint16_t line = 65535;
    std::vector<std::uint16_t> plane(static_cast<std::size_t>(size) * size, field);
    for (std::uint32_t i = 0; i < size; ++i) {
        for (const std::uint32_t at : {2u, size - 3u}) {
            plane[static_cast<std::size_t>(at) * size + i] = line;
            plane[static_cast<std::size_t>(i) * size + at] = line;
        }
    }
    const double grey = field / 65535.0;

    REQUIRE(canvas.set("size", "30x30") == MP_OK);
    REQUIRE(canvas.configure(linear_mono(size, size)) == MP_OK);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());
    REQUIRE(stage.attach(canvas));

    const auto lowest = [&]() {
        REQUIRE(canvas.present(mono16(plane, size, size)) == MP_OK);
        const std::vector<double> got = red_of(canvas, 30, 30);
        return *std::min_element(got.begin(), got.end());
    };

    // The default: hermite down. Nothing below the field, anywhere.
    CHECK(stage.described("down") == "hermite");
    CHECK(stage.described("antiring") == "0.0000");
    CHECK(lowest() >= grey - 1e-5);

    // Lanczos: the halo. Recorded rather than assumed, because the fix is
    // only a fix if the fault is real.
    REQUIRE(stage.set("down", "lanczos") == MP_OK);
    const double halo = lowest();
    CHECK(halo < grey - 1e-3);

    // And gone, with the kernel unchanged.
    REQUIRE(stage.set("antiring", "1") == MP_OK);
    CHECK(lowest() >= grey - 1e-5);

    // Half of it leaves some of the halo: not exactly half, because the two
    // passes each pull their own axis back and the corner pixel is under
    // both, but well inside the two ends.
    REQUIRE(stage.set("antiring", "0.5") == MP_OK);
    const double half = lowest();
    CHECK(half > halo + 0.25 * (grey - halo));
    CHECK(half < grey - 0.25 * (grey - halo));

    // The interior of the field is the field, for every one of them: the
    // weights are normalised, so a flat field stays flat at every phase.
    REQUIRE(stage.set("antiring", "0") == MP_OK);
    for (const char* kernel : {"hermite", "lanczos", "spline36", "box", "catrom"}) {
        REQUIRE(stage.set("down", kernel) == MP_OK);
        REQUIRE(canvas.present(mono16(plane, size, size)) == MP_OK);
        const std::vector<double> got = red_of(canvas, 30, 30);
        for (std::uint32_t y = 8; y < 22; ++y) {
            for (std::uint32_t x = 8; x < 22; ++x) {
                INFO(kernel << " at " << x << "," << y);
                CHECK(got[static_cast<std::size_t>(y) * 30u + x] == Approx(grey).margin(2e-5));
            }
        }
    }
}

TEST_CASE("what a person can set on the scaler, and what it says back", "[vdsp][scale]")
{
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());
    REQUIRE(canvas.configure(linear_mono(16, 12)) == MP_OK);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());

    // The defaults: sharp up, ringless down.
    CHECK(stage.described("up") == "lanczos");
    CHECK(stage.described("up_lobes") == "3");
    CHECK(stage.described("down") == "hermite");
    CHECK(stage.described("light") == "linear");
    CHECK(stage.described("scale") == "no picture yet");
    CHECK(stage.described("trouble") == "nothing");

    for (const Kind k : k_kinds) {
        REQUIRE(stage.set("up", name_of(k)) == MP_OK);
        CHECK(stage.described("up") == name_of(k));
        REQUIRE(stage.set("down", name_of(k)) == MP_OK);
        CHECK(stage.described("down") == name_of(k));
    }
    CHECK(stage.set("up", "sharpest") == MP_ERR_INVALID);
    CHECK(stage.described("trouble").find("hermite") != std::string::npos);
    CHECK(stage.set("down", "") == MP_ERR_INVALID);

    // Lobes: a whole number from one up, with no ceiling.
    REQUIRE(stage.set("up_lobes", "2") == MP_OK);
    CHECK(stage.described("up_lobes") == "2");
    REQUIRE(stage.set("down_lobes", "4") == MP_OK);
    CHECK(stage.described("down_lobes") == "4");
    REQUIRE(stage.set("down_lobes", "1000") == MP_OK);
    CHECK(stage.described("down_lobes") == "1000");
    CHECK(stage.set("up_lobes", "0") == MP_ERR_INVALID);
    CHECK(stage.set("up_lobes", "three") == MP_ERR_INVALID);
    CHECK(stage.set("up_lobes", "2.5") == MP_ERR_INVALID);

    // The cubic's parameters, any number.
    REQUIRE(stage.set("up_b", "0") == MP_OK);
    REQUIRE(stage.set("up_c", "0.75") == MP_OK);
    CHECK(stage.described("up_b") == "0.0000");
    CHECK(stage.described("up_c") == "0.7500");
    REQUIRE(stage.set("down_b", "-0.5") == MP_OK);
    CHECK(stage.described("down_b") == "-0.5000");
    CHECK(stage.set("down_c", "half") == MP_ERR_INVALID);

    REQUIRE(stage.set("antiring", "0.8") == MP_OK);
    CHECK(stage.described("antiring") == "0.8000");
    CHECK(stage.set("antiring", "some") == MP_ERR_INVALID);

    for (const char* light : {"gamma", "sigmoid", "linear"}) {
        REQUIRE(stage.set("light", light) == MP_OK);
        CHECK(stage.described("light") == light);
    }
    CHECK(stage.set("light", "dark") == MP_ERR_INVALID);
    REQUIRE(stage.set("gamma", "2.4") == MP_OK);
    CHECK(stage.described("gamma") == "2.4000");
    CHECK(stage.set("gamma", "0") == MP_ERR_INVALID);
    REQUIRE(stage.set("sigmoid_center", "0.6") == MP_OK);
    REQUIRE(stage.set("sigmoid_slope", "8") == MP_OK);
    REQUIRE(stage.set("sigmoid_range", "6") == MP_OK);
    CHECK(stage.described("sigmoid_center") == "0.6000");
    CHECK(stage.described("sigmoid_slope") == "8.0000");
    CHECK(stage.described("sigmoid_range") == "6.0000");
    CHECK(stage.set("sigmoid_range", "-1") == MP_ERR_INVALID);
    CHECK(stage.set("nonsense", "1") == MP_ERR_UNSUPPORTED);
    CHECK(stage.set("up_nonsense", "1") == MP_ERR_UNSUPPORTED);

    // Every settable row says what kind of value it takes, in the grammar the
    // shell draws from, and the read-only ones say so.
    std::uint32_t rows = 0;
    std::uint32_t typed = 0;
    std::uint32_t read_only = 0;
    for (std::uint32_t i = 0;; ++i) {
        char row[512];
        if (stage.vtbl().describe(stage.handle(), i, row, sizeof row) != MP_OK) {
            break;
        }
        ++rows;
        const std::string line{row};
        if (line.find("(read only)") != std::string::npos) {
            ++read_only;
        } else if (std::count(line.begin(), line.end(), '\t') == 3) {
            ++typed;
        }
    }
    CHECK(rows == 17u);
    CHECK(read_only == 3u);
    CHECK(typed == 14u);

    // And the probe: this runs on the presenter's device or it does not run.
    std::uint32_t score = 0;
    REQUIRE(stage.vtbl().probe(MP_GRAPHICS_D3D11, &score) == MP_OK);
    CHECK(score > 0u);
    REQUIRE(stage.vtbl().probe(MP_GRAPHICS_NONE, &score) == MP_OK);
    CHECK(score == 0u);
    MpVideoDsp* none = nullptr;
    CHECK(stage.vtbl().open(nullptr, &none) == MP_ERR_UNSUPPORTED);
    CHECK(none == nullptr);
}

TEST_CASE("the light the passes run in changes the average, and changes nothing at one to one",
          "[vdsp][scale]")
{
    // A downscale in gamma light averages codes rather than light, so the
    // checkerboard comes out darker than its linear mean; a sigmoid inside
    // its range does something else again. Neither may change a picture that
    // is not being resampled, because there is no pass for them to run in.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module scale{MEDIAPERCH_VDSP_SCALE, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(scale.as<MpVideoDspVtbl>() != nullptr);
    Canvas canvas{*video.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 16;
    const std::uint32_t height = 16;
    constexpr std::uint16_t dark = 6554;   // 0.1
    constexpr std::uint16_t bright = 58982; // 0.9
    std::vector<std::uint16_t> plane(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            plane[static_cast<std::size_t>(y) * width + x] = ((x + y) & 1u) != 0 ? bright : dark;
        }
    }
    const double lo = dark / 65535.0;
    const double hi = bright / 65535.0;

    REQUIRE(canvas.set("size", "8x8") == MP_OK);
    REQUIRE(canvas.configure(linear_mono(width, height)) == MP_OK);
    Stage stage{*scale.as<MpVideoDspVtbl>(), canvas};
    REQUIRE(stage.ok());
    REQUIRE(stage.attach(canvas));
    REQUIRE(stage.set("down", "box") == MP_OK);

    const auto centre = [&]() {
        REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
        const std::vector<double> got = red_of(canvas, 8, 8);
        return got[4u * 8u + 4u];
    };
    CHECK(centre() == Approx((lo + hi) / 2.0).margin(2e-5));

    REQUIRE(stage.set("light", "gamma") == MP_OK);
    REQUIRE(stage.set("gamma", "2.2") == MP_OK);
    const double gamma_mean =
        std::pow((std::pow(lo, 1.0 / 2.2) + std::pow(hi, 1.0 / 2.2)) / 2.0, 2.2);
    CHECK(centre() == Approx(gamma_mean).margin(1e-4));
    CHECK(gamma_mean < (lo + hi) / 2.0 - 0.01);

    REQUIRE(stage.set("light", "sigmoid") == MP_OK);
    const double center = 0.75;
    const double slope = 6.5;
    const double offset = 1.0 / (1.0 + std::exp(slope * center));
    const double range = 1.0 / (1.0 + std::exp(slope * (center - 1.0))) - offset;
    const auto encode = [&](double x) {
        return center - std::log(1.0 / (x * range + offset) - 1.0) / slope;
    };
    const auto decode = [&](double y) {
        return (1.0 / (1.0 + std::exp(slope * (center - y))) - offset) / range;
    };
    const double sigmoid_mean = decode((encode(lo) + encode(hi)) / 2.0);
    CHECK(centre() == Approx(sigmoid_mean).margin(1e-4));

    // And at one to one there is no pass, whatever the light.
    REQUIRE(canvas.set("size", "native") == MP_OK);
    REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
    const std::vector<double> got = red_of(canvas, width, height);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == Approx(plane[i] / 65535.0).margin(1e-6));
    }
    CHECK(stage.described("cost") == "0 taps across, 0 down, 0 passes");
}

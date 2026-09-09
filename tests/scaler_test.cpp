// SPDX-License-Identifier: GPL-3.0-or-later
//
// The presenter's own resampling: where chroma sits, what the gamut does, the
// fetch a chain with no scaler leaves, and the crop -- held to references
// written from the published formulas rather than to the shader's own
// arithmetic.
//
// **What the HDR10 test patterns found.** A 4K checkerboard bilinearly
// downscaled into a 1080p window was moiré the size of a thumb; the one-pixel
// rulers along the edges of a 2.39:1 field came and went with the phase, and
// read as a picture that had been cropped; a colour-clipping pattern merged
// its red bars a third of the way up and its white ones never, because the
// compositor clipped each BT.709 component on its own. The picture's own
// resampling is a stage now (§9.11, tests/vdsp_scale_test.cpp), and what the
// presenter is held to here is what is left with it:
//
//   - at one to one every sample comes back as it went in;
//   - a chroma sample lands where the stream sites it, by the kernel and its
//     parameters, and a colour past the display's gamut keeps its luminance
//     and its gradation;
//   - the roll-off starts from the tighter of the two peaks a stream states;
//   - a texture larger than its picture is drawn cropped, at one to one and
//     through the fallback;
//   - with no scaler in the chain another size is the sampler's bilinear,
//     and the row says so.
//
// Everything renders off screen on WARP and reads back fp32, the way
// hdr_transfer_test.cpp does: pixels, not a screenshot somebody looks at.

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
// The standards' curves, for the gamut test
// --------------------------------------------------------------------------

double pq_to_nits(double e)
{
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(e, 1.0 / m2);
    const double num = std::max(p - c1, 0.0);
    const double den = c2 - c3 * p;
    return 10000.0 * std::pow(num / den, 1.0 / m1);
}

double nits_to_pq(double nits)
{
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

/// BT.2390-8's EETF, normalised to the source's range, on one value.
double tone_map_bt2390(double nits, double source_peak, double target_peak)
{
    const double max_source = nits_to_pq(source_peak);
    const double max_target = nits_to_pq(target_peak);
    if (max_target >= max_source || nits <= 0.0) {
        return nits;
    }
    const double e1 = nits_to_pq(nits) / max_source;
    const double mt = max_target / max_source;
    const double ks = 1.5 * mt - 0.5;
    if (e1 < ks) {
        return nits;
    }
    const double t = std::clamp((e1 - ks) / (1.0 - ks), 0.0, 1.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double knee = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
                        (-2.0 * t3 + 3.0 * t2) * mt;
    return pq_to_nits(std::min(knee, mt) * max_source);
}

// --------------------------------------------------------------------------
// An off-screen presenter, told what the display is
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
        // display stated rather than probed, so `sdr_scale` is 1 and the
        // roll-off is in the path wherever this runs.
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

/// The picture at one to one, as the presenter decodes it.
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

constexpr std::array<const char*, 10> k_kernel_names{
    "bilinear", "box",      "hermite",  "bicubic",  "catrom",
    "mitchell", "spline16", "spline36", "spline64", "lanczos"};

} // namespace

TEST_CASE("at one to one every sample comes back as it went in", "[video][scaler]")
{
    // One pass, `Load` at the pixel's own index, nothing between the sample
    // and the target but the range mapping -- which for full-range sixteen
    // bits is a division by 65535 and nothing else.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 16;
    const std::uint32_t height = 12;
    const std::vector<std::uint16_t> plane = random_plane(width, height, 7u);
    const std::vector<double> got = rendered_linear(canvas, plane, width, height);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == Approx(plane[i] / 65535.0).margin(1e-6));
    }
    CHECK(canvas.described("scale") == "16x12 1:1");
    CHECK(canvas.described("chain") == "none");
}

TEST_CASE("with no scaler in the chain, another size is the sampler's bilinear, and the row "
          "says so",
          "[video][scaler]")
{
    // **What is left when the scaler node is taken out** (§9.11): the first
    // pass stops at linear light at the source's size and the finish pass
    // samples it through the sampler's bilinear. At exactly two to one every
    // target pixel's centre sits on the boundary between four source
    // samples, and a bilinear at a boundary is their mean -- so a two-to-one
    // checkerboard is still a flat grey here, and at any other factor it is
    // not, which is the whole reason there is a stage.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
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
    REQUIRE(canvas.present(mono16(plane, width, height)) == MP_OK);
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const std::vector<float> got = canvas.pixels(w, h);
    REQUIRE(w == 16u);
    REQUIRE(h == 12u);
    REQUIRE(got.size() == 16u * 12u * 4u);
    for (std::size_t i = 0; i < got.size(); i += 4) {
        CHECK(got[i] == Approx(mean).margin(2e-5));
        CHECK(got[i + 1] == Approx(mean).margin(2e-5));
        CHECK(got[i + 2] == Approx(mean).margin(2e-5));
    }
    CHECK(canvas.described("scale") ==
          "32x24 -> 16x12, x0.500 by the presenter's bilinear fetch");
    CHECK(canvas.described("chain") == "none");
}

TEST_CASE("chroma is reconstructed where the stream sites it", "[video][scaler][yuv]")
{
    // An NV12 picture whose chroma steps from one value to another between
    // chroma samples 1 and 2. Sited left -- H.273's type 0, which every
    // MPEG-family codec means by silence -- chroma sample 2 sits on luma
    // sample 4, so with an interpolating kernel luma column 4 gets exactly the
    // value after the step and column 2 exactly the value before it, and
    // column 3, halfway, the midpoint. Sited centre, chroma sample 2 sits on
    // luma 4.5, and column 4 is short of it. A fetch through the sampler at
    // normalised coordinates was the centre answer for every stream.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    const std::uint32_t width = 8;
    const std::uint32_t height = 4;
    constexpr std::uint8_t k_y = 128;
    constexpr std::uint8_t k_before = 96;
    constexpr std::uint8_t k_after = 160;
    constexpr std::uint8_t k_v = 128;
    const auto frame_with = [&](std::uint8_t u_left, std::uint8_t u_right,
                                std::vector<std::uint8_t>& luma,
                                std::vector<std::uint8_t>& chroma) {
        luma.assign(static_cast<std::size_t>(width) * height, k_y);
        chroma.resize(static_cast<std::size_t>(width / 2u) * (height / 2u) * 2u);
        for (std::uint32_t cy = 0; cy < height / 2u; ++cy) {
            for (std::uint32_t cx = 0; cx < width / 2u; ++cx) {
                const std::size_t at = (static_cast<std::size_t>(cy) * (width / 2u) + cx) * 2u;
                chroma[at] = cx < 2u ? u_left : u_right;
                chroma[at + 1u] = k_v;
            }
        }
        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout = MP_LAYOUT_NV12;
        frame.width = width;
        frame.height = height;
        frame.plane[0] = luma.data();
        frame.stride[0] = width;
        frame.plane[1] = chroma.data();
        frame.stride[1] = width;
        return frame;
    };

    MpVideoInfo info = linear_mono(width, height);
    info.flags = 0; // studio range, as video is
    REQUIRE(canvas.set("size", "native") == MP_OK);
    REQUIRE(canvas.set("chroma", "catrom") == MP_OK);
    REQUIRE(canvas.configure(info) == MP_OK);

    // The blue channel is affine in Cb, so the two flat pictures give the two
    // ends of the step and the stepped picture is read against them.
    const auto blue_at = [&](std::uint32_t x, std::uint32_t y) {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        const std::vector<float> got = canvas.pixels(w, h);
        REQUIRE(w == width);
        REQUIRE(h == height);
        return static_cast<double>(got[(static_cast<std::size_t>(y) * width + x) * 4u + 2u]);
    };
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
    REQUIRE(canvas.present(frame_with(k_before, k_before, luma, chroma)) == MP_OK);
    const double low = blue_at(4, 1);
    REQUIRE(canvas.present(frame_with(k_after, k_after, luma, chroma)) == MP_OK);
    const double high = blue_at(4, 1);
    REQUIRE(high > low + 0.3);

    SECTION("sited left, which is the default")
    {
        REQUIRE(canvas.set("siting", "left") == MP_OK);
        REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
        CHECK(blue_at(2, 1) == Approx(low).margin(1e-4));
        CHECK(blue_at(4, 1) == Approx(high).margin(1e-4));
        CHECK(blue_at(3, 1) == Approx((low + high) / 2.0).margin(1e-4));
        CHECK(canvas.described("siting") == "left");
    }
    SECTION("sited centre, a quarter of a luma sample away")
    {
        REQUIRE(canvas.set("siting", "centre") == MP_OK);
        REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
        const double at4 = blue_at(4, 1);
        CHECK(at4 < high - 0.1 * (high - low));
        CHECK(at4 > low + 0.5 * (high - low));
        CHECK(canvas.described("siting") == "centre");
    }
    SECTION("auto is the stream's, and the assumption when it said nothing")
    {
        REQUIRE(canvas.set("siting", "auto") == MP_OK);
        REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
        CHECK(blue_at(4, 1) == Approx(high).margin(1e-4));
        CHECK(canvas.described("siting") == "auto (left, assumed)");

        info.chroma_siting = 2; // H.273 type 1, centre, plus one
        REQUIRE(canvas.configure(info) == MP_OK);
        REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
        CHECK(blue_at(4, 1) < high - 0.1 * (high - low));
        CHECK(canvas.described("siting") == "auto (centre, the stream's)");
    }
    SECTION("the kernel's parameters reach the reconstruction")
    {
        // The parameterised cubic at Catmull-Rom's B and C is Catmull-Rom:
        // the same three columns as the interpolating kernel above. Hermite,
        // B=0 C=0, is interpolating too, so columns 2 and 4 hold, and its
        // midpoint is the same midpoint -- a step is symmetric.
        REQUIRE(canvas.set("siting", "left") == MP_OK);
        for (const char* kernel : {"bicubic", "hermite", "spline16", "lanczos"}) {
            INFO(kernel);
            REQUIRE(canvas.set("chroma", kernel) == MP_OK);
            REQUIRE(canvas.set("chroma_b", "0") == MP_OK);
            REQUIRE(canvas.set("chroma_c", "0.5") == MP_OK);
            REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
            CHECK(blue_at(2, 1) == Approx(low).margin(1e-4));
            CHECK(blue_at(4, 1) == Approx(high).margin(1e-4));
            CHECK(blue_at(3, 1) == Approx((low + high) / 2.0).margin(1e-4));
        }
        // Mitchell is not interpolating: column 4 is short of the step's top.
        REQUIRE(canvas.set("chroma", "mitchell") == MP_OK);
        REQUIRE(canvas.present(frame_with(k_before, k_after, luma, chroma)) == MP_OK);
        CHECK(blue_at(4, 1) < high - 0.02 * (high - low));
    }
}

TEST_CASE("past the display's gamut a colour keeps its luminance, and the bars stay apart",
          "[video][scaler][hdr]")
{
    // The colour-clipping pattern: bars of one primary at rising nits on a
    // field of that primary at the peak. A BT.2020 red past a third of the
    // way up is outside BT.709 -- its red component past 1 after the gamut
    // matrix -- and clipping the component merges every bar above that level
    // into one. The presenter desaturates towards grey at constant luminance
    // instead, so each bar is a step in brightness the display can show and
    // the series stays strictly increasing to the source's peak.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    MpVideoInfo info{};
    info.size = sizeof(info);
    info.width = 4;
    info.height = 4;
    info.display_width = 4;
    info.display_height = 4;
    info.primaries = 9; // BT.2020
    info.transfer = 16; // PQ
    info.matrix = 2;
    info.timescale = 24000;
    REQUIRE(canvas.set("size", "native") == MP_OK);
    REQUIRE(canvas.configure(info) == MP_OK);
    const double source = std::strtod(canvas.described("target").c_str() + 0, nullptr);
    REQUIRE(source == Approx(203.0));
    const std::string row = canvas.described("target");
    const double source_peak = std::strtod(row.c_str() + row.find("from ") + 5, nullptr);
    REQUIRE(source_peak == Approx(1000.0));

    std::vector<std::uint8_t> bgra(4u * 4u * 4u);
    const auto present_red = [&](std::uint8_t code) {
        for (std::size_t at = 0; at + 3 < bgra.size(); at += 4) {
            bgra[at] = 0;
            bgra[at + 1] = 0;
            bgra[at + 2] = code;
            bgra[at + 3] = 0xff;
        }
        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout = MP_LAYOUT_BGRA8;
        frame.width = 4;
        frame.height = 4;
        frame.plane[0] = bgra.data();
        frame.stride[0] = 16;
        return canvas.present(frame);
    };
    const auto pixel = [&]() {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        const std::vector<float> got = canvas.pixels(w, h);
        REQUIRE(got.size() >= 4u);
        return std::array<double, 3>{got[0], got[1], got[2]};
    };
    const double nits[] = {240, 300, 375, 450, 530, 620, 710, 820, 900, 950, 1000};

    SECTION("desaturated, which is the default")
    {
        CHECK(canvas.described("gamut") == "desaturate");
        double previous = -1.0;
        for (const double target_nits : nits) {
            const auto code = static_cast<std::uint8_t>(std::lround(nits_to_pq(target_nits) * 255.0));
            INFO(target_nits << " nits, code " << static_cast<int>(code));
            REQUIRE(present_red(code) == MP_OK);
            const std::array<double, 3> got = pixel();
            // Inside the display's gamut, all three.
            for (const double c : got) {
                CHECK(c >= -1e-4);
                CHECK(c <= 1.0 + 1e-4);
            }
            // The luminance the grade put there: BT.2020's red weight of the
            // tone-mapped red, which is what the compositor's clip threw away.
            const double red = pq_to_nits(code / 255.0);
            const double mapped = tone_map_bt2390(red, source_peak, 203.0) / 203.0;
            const double want_y = 0.2627 * mapped;
            const double got_y = 0.2126 * got[0] + 0.7152 * got[1] + 0.0722 * got[2];
            CHECK(got_y == Approx(want_y).margin(2e-3));
            // Still red: the red channel leads.
            CHECK(got[0] > got[1]);
            CHECK(got[0] > got[2]);
            // And every bar brighter than the one before it -- strictly up to
            // where BT.2390's curve flattens towards the source's peak, and
            // never darker by more than the eight-bit codes' worth after it:
            // from 1000 nits to 203 the curve puts 620 nits at 202.3 and 820
            // at 203.0, which is the roll-off's own shape and not the gamut's.
            // The gamut's part is that the *red* bars are held to this at
            // all: clipped per component they would have merged at 150.
            if (target_nits <= 620.0) {
                CHECK(got_y > previous + 1e-3);
            } else {
                CHECK(got_y > previous - 1e-3);
            }
            previous = got_y;
        }
    }
    SECTION("clipped, which is what happened before there was a choice")
    {
        REQUIRE(canvas.set("gamut", "clip") == MP_OK);
        CHECK(canvas.described("gamut") == "clip");
        const auto top = static_cast<std::uint8_t>(std::lround(nits_to_pq(1000.0) * 255.0));
        REQUIRE(present_red(top) == MP_OK);
        const std::array<double, 3> got = pixel();
        // Past 1, which the fp32 target keeps and a display would clip: the
        // BT.2020 red, moved to BT.709, is 1.66 of the display's red.
        CHECK(got[0] > 1.5);
        CHECK(got[1] < 0.0);
    }
}

TEST_CASE("the roll-off starts from the tighter of the two peaks the stream states",
          "[video][scaler][hdr]")
{
    // A mastering display of 4000 nits with nothing above 1000 on it is a
    // 1000-nit picture: the content light level is the brightest pixel there
    // is, and when it is stated and lower it is the range to normalise to.
    // The test pattern set has a folder for every combination.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    MpVideoInfo info{};
    info.size = sizeof(info);
    info.width = 4;
    info.height = 4;
    info.display_width = 4;
    info.display_height = 4;
    info.primaries = 9;
    info.transfer = 16;
    info.matrix = 2;
    info.timescale = 24000;
    const auto from = [&](std::uint32_t mastering_nits, std::uint32_t max_cll) {
        info.mastering_max_luminance = mastering_nits * 10000u;
        info.max_content_light_level = max_cll;
        REQUIRE(canvas.configure(info) == MP_OK);
        const std::string row = canvas.described("target");
        const std::size_t at = row.find("from ");
        REQUIRE(at != std::string::npos);
        return std::strtod(row.c_str() + at + 5, nullptr);
    };
    CHECK(from(4000, 1000) == Approx(1000.0));
    CHECK(from(1000, 4000) == Approx(1000.0));
    CHECK(from(0, 800) == Approx(800.0));
    CHECK(from(0, 0) == Approx(1000.0));
    CHECK(from(4000, 0) == Approx(4000.0));
}

TEST_CASE("a texture larger than the picture is drawn cropped, not whole", "[video][scaler][yuv]")
{
    // A decoder's texture is the coded frame -- 1608 rows for a 1606-row
    // picture -- and its frame states the picture. The rows past it are the
    // encoder's padding and must not reach the target, at one to one or
    // through the fallback's fetch, which clamps to the picture and not to
    // the texture because the intermediate it samples *is* the picture. Made
    // by hand on the presenter's own device, as the adopted-texture test
    // does, with the padding a different value.
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    MpVideoInfo info = linear_mono(24, 24);
    info.flags = 0;
    REQUIRE(canvas.set("size", "24x24") == MP_OK);
    REQUIRE(canvas.configure(info) == MP_OK);

    MpGraphicsDevice graphics{};
    graphics.size = sizeof(graphics);
    REQUIRE(canvas.vtbl().get_device(canvas.handle(), &graphics) == MP_OK);
    auto* device = static_cast<ID3D11Device*>(graphics.device);
    REQUIRE(device != nullptr);
    UINT support = 0;
    REQUIRE(SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)));
    REQUIRE((support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0);

    constexpr std::uint8_t k_picture = 120;
    constexpr std::uint8_t k_padding = 235;
    std::vector<std::uint8_t> nv12(32u * 32u + 16u * 32u, 128);
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            nv12[static_cast<std::size_t>(y) * 32u + x] = x < 24 && y < 24 ? k_picture : k_padding;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA seed{};
    seed.pSysMem = nv12.data();
    seed.SysMemPitch = 32;
    ID3D11Texture2D* texture = nullptr;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &seed, &texture)));

    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = MP_LAYOUT_NV12;
    frame.width = 24;
    frame.height = 24;
    frame.texture = texture;
    frame.texture_index = 0;

    const auto all_one_value = [&](std::uint32_t width, std::uint32_t height) {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        const std::vector<float> got = canvas.pixels(w, h);
        REQUIRE(w == width);
        REQUIRE(h == height);
        REQUIRE(got.size() == static_cast<std::size_t>(width) * height * 4u);
        double lowest = 1e9;
        double highest = -1e9;
        for (std::size_t i = 0; i < got.size(); i += 4) {
            lowest = std::min(lowest, static_cast<double>(got[i]));
            highest = std::max(highest, static_cast<double>(got[i]));
        }
        CHECK(highest - lowest < 1e-5);
        return lowest;
    };
    // The picture's own value, studio range, through a linear transfer --
    // in the red channel, which carries the one-part-in-a-thousand by which
    // a chroma code of 128 is not the middle of 16..240 (BT.709's Kr).
    const double u = (128.0 / 255.0 - 0.5) * (255.0 / 224.0);
    const double want = (k_picture - 16.0) / 219.0 + 2.0 * (1.0 - 0.2126) * u;

    REQUIRE(canvas.present(frame) == MP_OK);
    CHECK(all_one_value(24, 24) == Approx(want).margin(1e-4));
    CHECK(canvas.described("scale") == "24x24 1:1");

    // And through the fallback's fetch, which clamps its edge to the picture
    // and not to the texture.
    REQUIRE(canvas.set("size", "48x36") == MP_OK);
    REQUIRE(canvas.present(frame) == MP_OK);
    CHECK(all_one_value(48, 36) == Approx(want).margin(1e-4));
    CHECK(canvas.described("scale") ==
          "24x24 -> 48x36, x2.000 by the presenter's bilinear fetch");

    texture->Release();
}

TEST_CASE("what a person can set for the chroma reconstruction, and what it reports back",
          "[video][scaler]")
{
    Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);
    Canvas canvas{*module.as<MpVideoVtbl>()};
    REQUIRE(canvas.ok());

    CHECK(canvas.described("chroma") == "lanczos");
    CHECK(canvas.described("chroma_lobes") == "3");
    CHECK(canvas.described("gamut") == "desaturate");
    CHECK(canvas.described("scale") == "no frame yet");
    // **The scaler's keys are the stage's now** (§9.11): a presenter asked
    // for one answers as it answers any key it does not have.
    CHECK(canvas.described("scaler").empty());
    CHECK(canvas.set("scaler", "box") == MP_ERR_UNSUPPORTED);
    for (const char* name : k_kernel_names) {
        REQUIRE(canvas.set("chroma", name) == MP_OK);
        CHECK(canvas.described("chroma") == name);
    }
    CHECK(canvas.set("chroma", "sharpest") == MP_ERR_INVALID);
    CHECK(canvas.set("chroma", "") == MP_ERR_INVALID);
    REQUIRE(canvas.set("chroma_lobes", "2") == MP_OK);
    CHECK(canvas.described("chroma_lobes") == "2");
    REQUIRE(canvas.set("chroma_lobes", "1000") == MP_OK);
    CHECK(canvas.described("chroma_lobes") == "1000");
    CHECK(canvas.set("chroma_lobes", "0") == MP_ERR_INVALID);
    CHECK(canvas.set("chroma_lobes", "many") == MP_ERR_INVALID);
    REQUIRE(canvas.set("chroma_b", "0.25") == MP_OK);
    CHECK(canvas.described("chroma_b") == "0.2500");
    REQUIRE(canvas.set("chroma_c", "0.75") == MP_OK);
    CHECK(canvas.described("chroma_c") == "0.7500");
    CHECK(canvas.set("chroma_c", "some") == MP_ERR_INVALID);
    for (const char* name : {"left", "centre", "topleft", "top", "bottomleft", "bottom"}) {
        REQUIRE(canvas.set("siting", name) == MP_OK);
        CHECK(canvas.described("siting") == name);
    }
    REQUIRE(canvas.set("siting", "center") == MP_OK);
    CHECK(canvas.described("siting") == "centre");
    CHECK(canvas.set("siting", "middle") == MP_ERR_INVALID);
    REQUIRE(canvas.set("gamut", "clip") == MP_OK);
    CHECK(canvas.described("gamut") == "clip");
    CHECK(canvas.set("gamut", "squash") == MP_ERR_INVALID);
}

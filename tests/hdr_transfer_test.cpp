// SPDX-License-Identifier: GPL-3.0-or-later
//
// **The HDR curves, held to the standards that define them.**
//
// A colour pipeline judged by looking at it is a colour pipeline nobody can
// change, and §9.2 is a whole section about a shipping tone mapper that looks
// fine and is wrong. So none of these compares a picture with a picture: each
// puts a ramp through the *real* pixel shader, off-screen on WARP, and compares
// what comes back with the curve computed here from ST.2084, ARIB STD-B67 and
// BT.2390.
//
// **None of it needs an HDR display**, which is the whole reason it is a test
// rather than a ritual: the presenter renders into a texture with no window,
// `read_back` hands over the floats, and the arithmetic is the same on any
// machine.

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
#include <string>
#include <vector>

using Catch::Approx;

namespace {

// ---------------------------------------------------------------- the curves
//
// Written from the standards, not from the shader. Two copies of one formula is
// the point: if they drift, one of them is wrong and the test says so.

/// SMPTE ST.2084's EOTF: a code value in [0,1] to absolute luminance in nits.
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

/// ARIB STD-B67's inverse OETF, giving a scene-referred signal in [0,1].
double hlg_to_scene(double e)
{
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    return e <= 0.5 ? (e * e) / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
}

/// And its OOTF, which is what makes HLG a different picture on two displays.
struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

Rgb hlg_to_nits(const Rgb& coded, double peak)
{
    const Rgb scene{hlg_to_scene(coded.r), hlg_to_scene(coded.g), hlg_to_scene(coded.b)};
    const double y = 0.2627 * scene.r + 0.6780 * scene.g + 0.0593 * scene.b;
    const double gamma = 1.2 + 0.42 * std::log10(peak / 1000.0);
    const double scale = peak * std::pow(y, gamma - 1.0);
    return Rgb{scale * scene.r, scale * scene.g, scale * scene.b};
}

/// BT.2390's EETF, in the PQ domain, as the shader applies it.
///
/// **Written here from the recommendation, not from the shader**, which is the
/// point of having two copies: if they drift, one is wrong and this says so.
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

/// BT.2390-8, section 5.4.1: the source's range is normalised to [0, 1] first,
/// the knee sits at 1.5 * maxT - 0.5 with maxT the target's peak as a fraction
/// of the source's, the Hermite spline runs from the knee to maxT, and the
/// result is de-normalised. Black stays at zero: the recommendation's lift is
/// for a display that cannot reach it, and none of these can be asked to.
double eetf_bt2390(double e, double max_source, double max_target)
{
    const double e1 = e / max_source;
    const double mt = max_target / max_source;
    const double ks = 1.5 * mt - 0.5;
    if (e1 < ks) {
        return e;
    }
    const double t = std::clamp((e1 - ks) / (1.0 - ks), 0.0, 1.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double knee = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
                        (-2.0 * t3 + 3.0 * t2) * mt;
    return std::min(knee, mt) * max_source;
}

/// The EETF on a luminance, in nits: from a source graded to `source_peak`,
/// aimed at `peak`. A source the target can show whole is untouched.
double tone_map_bt2390(double nits, double source_peak, double peak)
{
    const double max_source = nits_to_pq(source_peak);
    const double max_target = nits_to_pq(peak);
    if (max_target >= max_source) {
        return nits;
    }
    return pq_to_nits(eetf_bt2390(nits_to_pq(nits), max_source, max_target));
}

/// The EETF on a colour: on its brightest component, the other two in ratio,
/// which is BT.2390's hue-preserving form and what the shader does.
Rgb tone_map_rgb(const Rgb& nits, double source_peak, double peak)
{
    const double brightest = std::max(nits.r, std::max(nits.g, nits.b));
    if (brightest <= 0.0) {
        return nits;
    }
    const double ratio = tone_map_bt2390(brightest, source_peak, peak) / brightest;
    return Rgb{nits.r * ratio, nits.g * ratio, nits.b * ratio};
}

/// RGB to XYZ from a set of chromaticities and a white point, as SMPTE RP 177
/// derives it; BT.2020 to BT.709 is then XYZ back out through the other set.
/// Written from the two recommendations' chromaticity tables and nothing else.
using Mat3 = std::array<std::array<double, 3>, 3>;

Mat3 rgb_to_xyz(double xr, double yr, double xg, double yg, double xb, double yb, double xw,
                double yw)
{
    const Mat3 p{{{xr / yr, xg / yg, xb / yb},
                  {1.0, 1.0, 1.0},
                  {(1.0 - xr - yr) / yr, (1.0 - xg - yg) / yg, (1.0 - xb - yb) / yb}}};
    const double wx = xw / yw;
    const double wz = (1.0 - xw - yw) / yw;
    // Solve p * s = w for the three scales.
    const double det = p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) -
                       p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                       p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
    const auto minor = [&](int r0, int c0) {
        int rows[2];
        int cols[2];
        int ri = 0;
        int ci = 0;
        for (int i = 0; i < 3; ++i) {
            if (i != r0) {
                rows[ri++] = i;
            }
            if (i != c0) {
                cols[ci++] = i;
            }
        }
        return p[rows[0]][cols[0]] * p[rows[1]][cols[1]] -
               p[rows[0]][cols[1]] * p[rows[1]][cols[0]];
    };
    Mat3 inv{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            inv[j][i] = ((i + j) % 2 == 0 ? 1.0 : -1.0) * minor(i, j) / det;
        }
    }
    const double s0 = inv[0][0] * wx + inv[0][1] * 1.0 + inv[0][2] * wz;
    const double s1 = inv[1][0] * wx + inv[1][1] * 1.0 + inv[1][2] * wz;
    const double s2 = inv[2][0] * wx + inv[2][1] * 1.0 + inv[2][2] * wz;
    Mat3 m{};
    for (int i = 0; i < 3; ++i) {
        m[i][0] = p[i][0] * s0;
        m[i][1] = p[i][1] * s1;
        m[i][2] = p[i][2] * s2;
    }
    return m;
}

Mat3 invert(const Mat3& a)
{
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    Mat3 out{};
    out[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    out[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    out[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    return out;
}

/// BT.2020 to BT.709, from the chromaticities: BT.2020's (0.708, 0.292),
/// (0.170, 0.797), (0.131, 0.046); BT.709's (0.640, 0.330), (0.300, 0.600),
/// (0.150, 0.060); D65 (0.3127, 0.3290) for both.
Mat3 bt2020_to_bt709_derived()
{
    const Mat3 to_xyz = rgb_to_xyz(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290);
    const Mat3 from_xyz =
        invert(rgb_to_xyz(0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290));
    Mat3 out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i][j] = from_xyz[i][0] * to_xyz[0][j] + from_xyz[i][1] * to_xyz[1][j] +
                        from_xyz[i][2] * to_xyz[2][j];
        }
    }
    return out;
}

/// BT.2020's primaries into BT.709's, in linear light -- what scRGB needs.
Rgb bt2020_to_bt709(const Rgb& c)
{
    return Rgb{1.6605 * c.r - 0.5876 * c.g - 0.0728 * c.b,
               -0.1246 * c.r + 1.1329 * c.g - 0.0083 * c.b,
               -0.0182 * c.r - 0.1006 * c.g + 1.1187 * c.b};
}

// ------------------------------------------------------------- the presenter
//
// The same off-screen WARP harness `video_d3d11_test.cpp` uses, kept here
// rather than shared because sharing it would mean a header for two tests and
// the thing being shared is thirty lines.

class Presenter {
public:
    Presenter(const MpVideoVtbl& vtbl, std::uint32_t width, std::uint32_t height)
        : vtbl_(&vtbl), width_(width), height_(height)
    {
        if (vtbl_->open(nullptr, &handle_) != MP_OK) {
            handle_ = nullptr;
            return;
        }
        // WARP, because a test that renders on whatever GPU is in the machine
        // is a test that can only be trusted on that machine.
        ok_ = vtbl_->set(handle_, "device", "warp") == MP_OK;

        info_ = MpVideoInfo{};
        info_.size = sizeof(info_);
        info_.width = width;
        info_.height = height;
        info_.display_width = width;
        info_.display_height = height;
        info_.primaries = 9;  // BT.2020, which is what both HDR curves are on
        info_.transfer = 16;  // ST.2084
        info_.matrix = 1;
        info_.timescale = 24000;
    }
    ~Presenter()
    {
        if (handle_ != nullptr) {
            vtbl_->close(handle_);
        }
    }
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr && ok_; }
    MpVideoInfo& info() noexcept { return info_; }

    /// **What peak the plan is using**, read out of the display row rather
    /// than assumed.
    ///
    /// An off-screen presenter has no window, and §9.4's rule then falls back
    /// to the first output -- a real monitor with a real peak, not the 1000
    /// nits BT.2100 assumes when nothing says otherwise. The HLG OOTF is a
    /// function of that number, so a test that assumed it would be testing this
    /// machine's panel instead of the shader.
    /// The display's SDR white, which is what an SDR display's peak is and
    /// what §9's shader tone-maps towards.
    [[nodiscard]] double white_nits() const
    {
        const std::string row = described("display");
        const std::size_t at = row.find("white ");
        if (at == std::string::npos) {
            return 0.0;
        }
        return std::strtod(row.c_str() + at + 6, nullptr);
    }

    [[nodiscard]] double peak_nits() const
    {
        const std::string row = described("display");
        const std::size_t at = row.find("peak ");
        if (at == std::string::npos) {
            return 0.0;
        }
        return std::strtod(row.c_str() + at + 5, nullptr);
    }

    /// Where the shader's roll-off aims and where it starts, off the `target`
    /// row: "203 nits from 1000". Zero when nothing is mapped.
    [[nodiscard]] double target_nits() const
    {
        return std::strtod(described("target").c_str(), nullptr);
    }
    [[nodiscard]] double source_nits() const
    {
        const std::string row = described("target");
        const std::size_t at = row.find("from ");
        if (at == std::string::npos) {
            return 0.0;
        }
        return std::strtod(row.c_str() + at + 5, nullptr);
    }

    /// §9.7.1's surface, asked for before `configure`.
    [[nodiscard]] MpResult surface(const char* which)
    {
        return vtbl_->set(handle_, "surface", which);
    }

    /// The tone mapper by name, so a test measuring a transfer can say that it
    /// is measuring only a transfer.
    [[nodiscard]] MpResult tonemap(const char* name)
    {
        return vtbl_->set(handle_, "tonemap", name);
    }

    [[nodiscard]] std::string configure()
    {
        const MpResult r = vtbl_->configure(handle_, &info_);
        return r == MP_OK ? std::string{}
                          : "MpResult " + std::to_string(static_cast<unsigned>(r));
    }

    [[nodiscard]] std::string described(const char* key) const
    {
        for (std::uint32_t i = 0;; ++i) {
            char row[256];
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

    /// One flat grey at `bits` a sample, as a single luma plane.
    ///
    /// **This is the shape HDR is actually coded in.** PQ at eight bits is a
    /// format nobody ships: HDR10 is ten, and the twelve-bit case is where the
    /// arithmetic has to be right or the sky bands.
    ///
    /// **4:0:0 on purpose.** A neutral chroma is not exactly a half: the code
    /// at the middle of a range is `1 << (bits-1)` and the range is
    /// `(1 << bits) - 1`, so a "grey" 4:2:0 frame is one part in a thousand off
    /// neutral and the green channel carries the difference. That is true of
    /// real content and is a matrix question rather than a transfer one, so a
    /// test *about the transfer* takes the chroma out and lets the shader's
    /// `has_chroma` be zero.
    [[nodiscard]] MpResult present_grey(std::uint32_t code, std::uint32_t bits)
    {
        const std::uint32_t shift = 16u - bits;
        std::vector<std::uint16_t> luma(static_cast<std::size_t>(width_) * height_,
                                        static_cast<std::uint16_t>(code << shift));

        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout.size = sizeof(frame.layout);
        frame.layout.chroma = MP_CHROMA_MONO;
        frame.layout.packing = MP_PACK_PLANAR;
        frame.layout.bits = bits;
        frame.layout.container_bits = 16;
        // **Where the bits sit inside the container**, which is what P010 says
        // and what `mp_pixel_sample_scale` turns into the shader's multiplier.
        frame.layout.shift = shift;
        frame.width = width_;
        frame.height = height_;
        frame.plane[0] = luma.data();
        frame.stride[0] = static_cast<std::uint32_t>(width_) * 2u;
        frame.pts = 0;
        return vtbl_->present(handle_, &frame);
    }

    /// `fp32` measures the pipeline, `fp16` measures what a display gets.
    [[nodiscard]] MpResult precision(const char* which)
    {
        return vtbl_->set(handle_, "precision", which);
    }

    /// One flat colour, as BGRA8, through the whole path.
    [[nodiscard]] MpResult present(std::uint8_t b, std::uint8_t g, std::uint8_t r)
    {
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width_) * height_ * 4);
        for (std::size_t at = 0; at + 3 < bgra.size(); at += 4) {
            bgra[at] = b;
            bgra[at + 1] = g;
            bgra[at + 2] = r;
            bgra[at + 3] = 0xff;
        }
        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        // Spelled by the ABI's own macro, so a test states the same thing a
        // decoder would rather than a second description of it.
        frame.layout = MP_LAYOUT_BGRA8;
        frame.width = width_;
        frame.height = height_;
        frame.plane[0] = bgra.data();
        frame.stride[0] = width_ * 4u;
        frame.pts = 0;
        return vtbl_->present(handle_, &frame);
    }

    /// Every pixel, so a half-precision read can say what it quantised to.
    [[nodiscard]] std::vector<float> all()
    {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        MpPixelLayout layout{};
        layout.size = sizeof(layout);
        if (vtbl_->read_back(handle_, nullptr, 0, &w, &h, &layout) != MP_ERR_NO_MEMORY) {
            return {};
        }
        const std::size_t count = static_cast<std::size_t>(w) * h * 4u;
        if ((layout.flags & MP_PIXEL_FLOAT) == 0u) {
            return {};
        }
        if (layout.container_bits == 32u) {
            std::vector<float> out(count);
            if (vtbl_->read_back(handle_, out.data(), out.size() * sizeof(float), &w, &h,
                                 &layout) != MP_OK) {
                return {};
            }
            return out;
        }
        std::vector<std::uint16_t> halves(count);
        if (vtbl_->read_back(handle_, halves.data(), halves.size() * sizeof(std::uint16_t),
                             &w, &h, &layout) != MP_OK) {
            return {};
        }
        std::vector<float> out(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t v = halves[i];
            const std::uint32_t sign = static_cast<std::uint32_t>(v & 0x8000u) << 16;
            const std::uint32_t exponent = (v >> 10) & 0x1Fu;
            const std::uint32_t mantissa = v & 0x3FFu;
            std::uint32_t bits = sign;
            if (exponent == 0 && mantissa == 0) {
                // zero
            } else if (exponent == 31) {
                bits |= 0x7F800000u | (mantissa << 13);
            } else if (exponent == 0) {
                float sub = static_cast<float>(mantissa) / 1024.0f / 16384.0f;
                out[i] = sign != 0 ? -sub : sub;
                continue;
            } else {
                bits |= (exponent + 112u) << 23;
                bits |= mantissa << 13;
            }
            std::memcpy(&out[i], &bits, sizeof(float));
        }
        return out;
    }

    /// The top-left pixel, in linear scRGB.
    ///
    /// **Single precision off-screen**, which is what the presenter renders
    /// into with no window: a measurement quantised before it is taken is a
    /// measurement of the quantiser. The half-precision path is what a display
    /// gets and is asked for by name.
    [[nodiscard]] Rgb pixel()
    {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        MpPixelLayout layout{};
        layout.size = sizeof(layout);
        if (vtbl_->read_back(handle_, nullptr, 0, &w, &h, &layout) != MP_ERR_NO_MEMORY ||
            w == 0 || h == 0) {
            return {};
        }
        if ((layout.flags & MP_PIXEL_FLOAT) == 0u || layout.container_bits != 32u) {
            return {};
        }
        std::vector<float> out(static_cast<std::size_t>(w) * h * 4u);
        if (vtbl_->read_back(handle_, out.data(), out.size() * sizeof(float), &w, &h,
                             &layout) != MP_OK) {
            return {};
        }
        return Rgb{out[0], out[1], out[2]};
    }

    /// The top-left pixel whatever the target's width, which is what a
    /// measurement of the *format* rather than of the arithmetic needs.
    [[nodiscard]] Rgb pixel_any()
    {
        const std::vector<float> pixels = all();
        if (pixels.size() < 4) {
            return {};
        }
        return Rgb{pixels[0], pixels[1], pixels[2]};
    }

private:
    const MpVideoVtbl* vtbl_;
    MpVideo* handle_ = nullptr;
    bool ok_ = false;
    std::uint32_t width_;
    std::uint32_t height_;
    MpVideoInfo info_{};
};

/// fp16 has ten bits of mantissa, so a value near 100 is quantised to about
/// 0.05. Everything here is compared as a fraction rather than absolutely,
/// which is what makes one tolerance right at 0.001 nits and at 4000.
constexpr double k_tolerance = 0.01;

void close_enough(double got, double want)
{
    if (std::abs(want) < 1e-4) {
        CHECK(std::abs(got) < 1e-3);
        return;
    }
    CHECK(got == Catch::Approx(want).epsilon(k_tolerance));
}

} // namespace

TEST_CASE("PQ decodes to the nits ST.2084 says, through the real shader",
          "[video][hdr]")
{
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
    REQUIRE(presenter.ok());
    // **Ours, because the plan will not let a display be lied to.** HDR content
    // on an SDR display is the case §9 exists for, and `plan_for` turns a
    // request for `none` into `driver` rather than letting composition clip it
    // silently. So this measures the whole path a viewer would get -- the
    // transfer and the roll-off -- against both formulas rather than one.
    REQUIRE(presenter.tonemap("shader") == MP_OK);
    REQUIRE(presenter.configure() == "");
    CHECK(presenter.described("applied") == "shader");
    // BT.2408: on an SDR display the roll-off aims at HDR's reference white,
    // and a stream with no mastering display is taken as a 1000-nit grade.
    const double target = presenter.target_nits();
    const double source = presenter.source_nits();
    CHECK(target == Approx(203.0));
    CHECK(source == Approx(1000.0));
    REQUIRE(target > 0.0);

    // A ramp, and every step of it against the standard.
    for (const std::uint8_t code : {std::uint8_t{0}, std::uint8_t{32}, std::uint8_t{64},
                                    std::uint8_t{128}, std::uint8_t{192},
                                    std::uint8_t{255}}) {
        REQUIRE(presenter.present(code, code, code) == MP_OK);
        const Rgb got = presenter.pixel();

        const double nits = tone_map_bt2390(pq_to_nits(code / 255.0), source, target);
        // In units of the target white, and BT.2020 to BT.709 for a grey is
        // not identity: the matrix rows do not each sum to one.
        const Rgb want =
            bt2020_to_bt709(Rgb{nits / target, nits / target, nits / target});

        INFO("code " << static_cast<unsigned>(code) << " is " << nits << " nits");
        close_enough(got.r, want.r);
        close_enough(got.g, want.g);
        close_enough(got.b, want.b);
    }
}

TEST_CASE("HLG decodes through its OOTF at the display's peak", "[video][hdr]")
{
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
    REQUIRE(presenter.ok());
    presenter.info().transfer = 18;  // ARIB STD-B67
    REQUIRE(presenter.tonemap("shader") == MP_OK);
    REQUIRE(presenter.configure() == "");

    const double peak = presenter.peak_nits();
    const double target = presenter.target_nits();
    const double source = presenter.source_nits();
    REQUIRE(peak > 0.0);
    REQUIRE(target > 0.0);

    for (const std::uint8_t code :
         {std::uint8_t{64}, std::uint8_t{128}, std::uint8_t{192}, std::uint8_t{255}}) {
        REQUIRE(presenter.present(code, code, code) == MP_OK);
        const Rgb got = presenter.pixel();

        const double e = code / 255.0;
        const Rgb light = hlg_to_nits(Rgb{e, e, e}, peak);
        const Rgb nits = tone_map_rgb(light, source, target);
        const Rgb want =
            bt2020_to_bt709(Rgb{nits.r / target, nits.g / target, nits.b / target});

        INFO("code " << static_cast<unsigned>(code));
        close_enough(got.r, want.r);
        close_enough(got.g, want.g);
        close_enough(got.b, want.b);
    }
}

// **ASCII in the name, deliberately.** `catch_discover_tests` registers each
// case with ctest and ctest re-invokes the binary with the name as a filter;
// a non-ASCII byte does not survive that round trip, and the case then
// matches nothing and is reported as a failure that has nothing to do with
// what it tests. Section numbers go in the comment, not the title.
TEST_CASE("HLG is not PQ, which is the fault section 9.9.1 was written about",
          "[video][hdr]")
{
    // The two curves disagree by a lot at the same code value, which is exactly
    // why putting HLG code values into a PQ buffer produces a picture that is
    // far too dark and wrongly graded -- and why the plan has a `Convert`
    // separate from its `ToneMap`.
    // Half scale is 92 nits under ST.2084 and 51 under BT.2100 at the
    // reference 1000-nit display: not a rounding difference, a different
    // picture. Nothing about the failure would have pointed at the transfer.
    const double as_pq = pq_to_nits(0.5);
    const Rgb as_hlg = hlg_to_nits(Rgb{0.5, 0.5, 0.5}, 1000.0);
    CHECK(as_pq == Catch::Approx(92.2).epsilon(0.01));
    CHECK(as_hlg.g == Catch::Approx(50.7).epsilon(0.01));
}

TEST_CASE("a mastering display is absent until something states one", "[video][hdr]")
{
    // **Zero is not a mastering display at nought nits, it is the absence of
    // one**, and one question in one place is how that stays true: a presenter
    // that invented its own test would eventually invent a different one.
    MpVideoInfo info{};
    info.size = sizeof(info);
    CHECK(mp_video_has_mastering(&info) == 0);

    // BT.2020's primaries and D65, in ST.2086's own units -- 0.00002, so 0.708
    // is 35400. The numbers a real HDR10 file carries.
    info.mastering_primaries_x[0] = 35400;  // red x, 0.708
    info.mastering_primaries_y[0] = 14600;  // red y, 0.292
    info.mastering_primaries_x[1] = 8500;   // green x, 0.170
    info.mastering_primaries_y[1] = 39850;  // green y, 0.797
    info.mastering_primaries_x[2] = 6550;   // blue x, 0.131
    info.mastering_primaries_y[2] = 2300;   // blue y, 0.046
    info.mastering_white_x = 15635;         // D65 x, 0.3127
    info.mastering_white_y = 16450;         // D65 y, 0.3290
    info.mastering_max_luminance = 10000000;  // 1000 nits, in 0.0001 cd/m^2
    info.mastering_min_luminance = 1;         // 0.0001 nits
    CHECK(mp_video_has_mastering(&info) != 0);

    // **A caller from before the append says no**, whatever is behind its
    // pointer: `size` is the caller's own and the fields are not there to read.
    MpVideoInfo older = info;
    older.size = 48;
    CHECK(mp_video_has_mastering(&older) == 0);
    CHECK(mp_video_has_mastering(nullptr) == 0);
}

TEST_CASE("the providers that are not ours are held to properties, not values",
          "[video][hdr]")
{
    // **Their arithmetic is theirs.** Section 9.2 is a whole section about the
    // OS mapper being measurably wrong, so comparing d2d or driver against a
    // formula would be asserting that Microsoft agrees with a recommendation it
    // is known not to follow. What a test can say is that the path runs, that
    // what comes back is finite and in range, and that it does not invert the
    // picture -- brighter in, no darker out.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    for (const char* provider : {"d2d", "driver"}) {
        Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
        REQUIRE(presenter.ok());
        REQUIRE(presenter.tonemap(provider) == MP_OK);
        const std::string trouble = presenter.configure();
        INFO("provider " << provider);
        if (!trouble.empty()) {
            // A provider that will not open on this machine is a fact about the
            // machine. WARP has no driver video processor; it does have
            // Direct2D. Saying which is more useful than a skip.
            WARN("provider " << provider << " would not configure: " << trouble);
            continue;
        }

        double previous = -1.0;
        for (const std::uint8_t code : {std::uint8_t{16}, std::uint8_t{64},
                                        std::uint8_t{128}, std::uint8_t{255}}) {
            REQUIRE(presenter.present(code, code, code) == MP_OK);
            const Rgb got = presenter.pixel();
            INFO("code " << static_cast<unsigned>(code) << " gave " << got.g);
            CHECK(std::isfinite(got.r));
            CHECK(std::isfinite(got.g));
            CHECK(std::isfinite(got.b));
            CHECK(got.g >= 0.0);
            // **Monotone**, which is the one thing every tone mapper worth the
            // name does and the one a broken pass fails: a roll-off that
            // reordered two code values would not be a roll-off.
            CHECK(got.g >= previous - 1e-4);
            previous = got.g;
        }
        // And it fell back to something rather than presenting the unmapped
        // picture, which section 9.1 says composition would clip silently.
        const std::string applied = presenter.described("applied");
        CHECK((applied == provider || applied == "shader"));
    }
}

TEST_CASE("PQ and HLG at ten and twelve bits, which is how HDR is coded",
          "[video][hdr]")
{
    // **Eight-bit PQ is a format nobody ships.** HDR10 is ten bits and the
    // twelve-bit case is where the arithmetic has to be right or the sky bands,
    // so the tests above -- which go in as BGRA8 -- measure the curve on a
    // depth the curve is never used at. This one goes down the Y'CbCr path a
    // decoder's frame takes, at both depths that matter.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    struct Curve {
        std::uint32_t code_point;
        const char* name;
    };
    for (const Curve curve : {Curve{16, "PQ"}, Curve{18, "HLG"}}) {
        for (const std::uint32_t bits : {10u, 12u}) {
            Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
            REQUIRE(presenter.ok());
            presenter.info().transfer = curve.code_point;
            REQUIRE(presenter.tonemap("shader") == MP_OK);
            REQUIRE(presenter.configure() == "");

            const double peak = presenter.peak_nits();
            const double target = presenter.target_nits();
    const double source = presenter.source_nits();
            REQUIRE(peak > 0.0);
            REQUIRE(target > 0.0);

            const std::uint32_t top = (1u << bits) - 1u;
            for (const double fraction : {0.25, 0.5, 0.75, 1.0}) {
                const auto code = static_cast<std::uint32_t>(fraction * top);
                REQUIRE(presenter.present_grey(code, bits) == MP_OK);
                const Rgb got = presenter.pixel();

                // **Studio range**, which is what a Y'CbCr stream is unless it
                // says otherwise and what the presenter assumes: the shader
                // subtracts the offset and scales before the transfer, so the
                // expected value has to do the same.
                const double e = std::clamp(
                    (static_cast<double>(code) / top - 16.0 / 255.0) * (255.0 / 219.0), 0.0,
                    1.0);
                Rgb light;
                if (curve.code_point == 16) {
                    const double nits = tone_map_bt2390(pq_to_nits(e), source, target);
                    light = Rgb{nits, nits, nits};
                } else {
                    light = tone_map_rgb(hlg_to_nits(Rgb{e, e, e}, peak), source, target);
                }
                const Rgb want = bt2020_to_bt709(
                    Rgb{light.r / target, light.g / target, light.b / target});

                INFO(curve.name << " at " << bits << " bits, code " << code);
                close_enough(got.r, want.r);
                close_enough(got.g, want.g);
                close_enough(got.b, want.b);
            }
        }
    }
}

TEST_CASE("the arithmetic is wider than the format a display gets", "[video][hdr]")
{
    // **§9.10, measured rather than asserted.** The shader works in single
    // precision and a swap chain will not take it: DXGI offers 8-bit UNORM,
    // 10-bit UNORM and RGBA16F and nothing above. Half's relative step is
    // 1/1024 at worst, and a twelve-bit source needs 1/1706 at white -- so the
    // *pipeline* can carry twelve bits and the *presentation format* cannot,
    // and the difference between the two renders is exactly that gap.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    const auto render = [&](const char* precision, std::uint32_t code) {
        Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
        REQUIRE(presenter.ok());
        REQUIRE(presenter.precision(precision) == MP_OK);
        presenter.info().transfer = 16;  // PQ
        REQUIRE(presenter.tonemap("shader") == MP_OK);
        REQUIRE(presenter.configure() == "");
        REQUIRE(presenter.present_grey(code, 12) == MP_OK);
        return presenter.pixel_any().g;
    };

    // Two twelve-bit codes one step apart, **below the roll-off's knee** so
    // the curve is still one to one: above it BT.2390 flattens towards the
    // target and two adjacent codes can land on one float, rightly. Single
    // precision resolves them; half is asked whether it does.
    constexpr std::uint32_t code = 1800;
    const double wide_low = render("fp32", code);
    const double wide_high = render("fp32", code + 1);
    REQUIRE(wide_low > 0.0);
    // **The pipeline resolves one step of twelve bits.** If it did not, the
    // arithmetic would be the thing losing them rather than the format.
    CHECK(wide_high > wide_low);

    const double half_low = render("fp16", code);
    const double half_high = render("fp16", code + 1);
    REQUIRE(half_low > 0.0);
    // Half may or may not resolve it -- that is the point of measuring rather
    // than asserting -- but it must not disagree with single precision by more
    // than its own step, which is 2^-11 relative.
    const double step = std::abs(half_low - wide_low) / wide_low;
    INFO("fp16 " << half_low << " against fp32 " << wide_low);
    CHECK(step < 1.0 / 1024.0);
    CHECK(half_high >= half_low);
}

TEST_CASE("8K, which nothing here had ever been asked for", "[video][hdr][slow]")
{
    // **7680x4320 is 33 megapixels**, four times 4K and thirty-three times what
    // every other test in this file uses. Nothing in the presenter is written
    // against a size, and that is exactly the kind of claim that is true until
    // somebody tries: a texture that large is 530 MB at RGBA32F, which is where
    // a machine says no if it is going to.
    //
    // Tagged slow, so a run that wants to be quick can leave it out; it is in
    // the default set because a size nobody tries is a size that breaks in
    // front of a user.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    Presenter presenter{*module.as<MpVideoVtbl>(), 7680, 4320};
    REQUIRE(presenter.ok());
    // Half precision, because 530 MB of single is more than this test needs to
    // prove and is the difference between a second and a stall.
    REQUIRE(presenter.precision("fp16") == MP_OK);
    presenter.info().transfer = 16;  // PQ, so the deep path is what is exercised
    REQUIRE(presenter.tonemap("shader") == MP_OK);

    const std::string trouble = presenter.configure();
    if (!trouble.empty()) {
        WARN("8K would not configure on this machine: " << trouble);
        return;
    }
    const double target = presenter.target_nits();
    const double source = presenter.source_nits();
    REQUIRE(target > 0.0);

    // Ten-bit, which is what an 8K HDR stream is.
    constexpr std::uint32_t code = 700;
    REQUIRE(presenter.present_grey(code, 10) == MP_OK);

    const std::vector<float> pixels = presenter.all();
    REQUIRE(pixels.size() == 7680ull * 4320ull * 4ull);

    const double e = std::clamp((code / 1023.0 - 16.0 / 255.0) * (255.0 / 219.0), 0.0, 1.0);
    const double nits = tone_map_bt2390(pq_to_nits(e), source, target);
    const Rgb want = bt2020_to_bt709(Rgb{nits / target, nits / target, nits / target});

    // **Every pixel, not the first one.** A presenter that got the size wrong
    // draws a correct corner and a wrong edge, and a test that looked at one
    // pixel would agree with it. Half precision, so the tolerance is half's.
    std::size_t wrong = 0;
    for (std::size_t at = 0; at + 3 < pixels.size(); at += 4) {
        if (std::abs(pixels[at + 1] - want.g) > std::abs(want.g) * 0.01 + 1e-4) {
            ++wrong;
        }
    }
    INFO("expected " << want.g << ", first was " << pixels[1]);
    CHECK(wrong == 0);
}

TEST_CASE("10K, 12K and 16K, which is where the platform says no",
          "[video][hdr][slow]")
{
    // §9.8.2 argued that nothing above the decoder has a resolution in it and
    // that Direct3D 11's 16384 texture limit is comfortably past 16K DCI. This
    // asks. **Configure and present only**: reading a 16K frame back is a
    // gigabyte through the test's own hands and would measure the harness.
    //
    // Memory is what runs out, and it runs out predictably: the target is
    // width x height x 4 channels x 2 bytes at half precision, and there is a
    // staging texture of the same size behind it. 16K DCI is 1.06 GB each.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    struct Size {
        std::uint32_t width;
        std::uint32_t height;
        const char* name;
    };
    for (const Size size : {Size{10240, 5760, "10K"}, Size{12288, 6912, "12K"},
                            Size{15360, 8640, "16K DCI"}}) {
        Presenter presenter{*module.as<MpVideoVtbl>(), size.width, size.height};
        REQUIRE(presenter.ok());
        REQUIRE(presenter.precision("fp16") == MP_OK);
        presenter.info().transfer = 16;  // PQ, so the deep path is what is asked
        REQUIRE(presenter.tonemap("shader") == MP_OK);

        const std::string trouble = presenter.configure();
        INFO(size.name << " is " << size.width << "x" << size.height);
        if (!trouble.empty()) {
            // A machine that will not give two gigabytes of texture is a fact
            // about the machine, and saying which size it stopped at is more
            // useful than a skip.
            WARN(size.name << " would not configure: " << trouble);
            continue;
        }
        // Ten-bit, which is what a stream at these sizes would be.
        const MpResult shown = presenter.present_grey(700, 10);
        if (shown != MP_OK) {
            WARN(size.name << " configured and would not present: MpResult "
                           << static_cast<unsigned>(shown));
            continue;
        }
        CHECK(shown == MP_OK);
        CHECK(presenter.described("encoding") == "linear scRGB");
    }
}

TEST_CASE("the engine renders into a surface and still has no window",
          "[video][hdr]")
{
    // **§9.7.1's decision, exercised from the engine's side.** A headless
    // engine that created windows would not be headless, and a presenter
    // drawing into a window owned by a process that may be killed at any moment
    // has to survive that killing anyway -- so the frame crosses the boundary
    // instead of the window crossing it. The half this can test is the half
    // that lives here: a composition surface handle, a swap chain on it, and a
    // picture presented into it, with no HWND anywhere.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    Presenter presenter{*module.as<MpVideoVtbl>(), 64, 48};
    REQUIRE(presenter.ok());
    REQUIRE(presenter.surface("composition") == MP_OK);

    const std::string trouble = presenter.configure();
    if (!trouble.empty()) {
        WARN("no composition surface on this machine: " << trouble);
        return;
    }

    // The handle is reported as a number because that is what crosses a process
    // boundary: a shell duplicates it out of an IPC message and asks
    // DirectComposition for a surface over it.
    const std::string where = presenter.described("surface");
    INFO("surface row: " << where);
    CHECK(where.rfind("composition 0x", 0) == 0);
    CHECK(where.find("composition 0x0,") == std::string::npos);
    // **And a frame clock that needs no window.** `show` paces on WaitForVBlank
    // against the output its window is on; an engine has neither, and a
    // waitable chain hands back an event the compositor sets -- the same
    // question, answered by the thing that will actually show the frame.
    CHECK(where.find("waitable 0x") != std::string::npos);
    CHECK(where.find("waitable 0x0") == std::string::npos);

    // And it presents. Not read back: a flip-model chain's back buffer is the
    // shell's to composite and this process is deliberately not looking at it.
    REQUIRE(presenter.present(0x20, 0x40, 0x80) == MP_OK);
    CHECK(presenter.described("frames") == "1");
}

TEST_CASE("the reference curves land on the numbers the standards publish",
          "[video][hdr][reference]")
{
    // **The references above are what the shader is held to, so they are held
    // to something first**: values the recommendations state in words rather
    // than formulas, so a transcription error in a constant cannot pass by
    // agreeing with itself.
    //
    // ST.2084 / BT.2100: PQ 1.0 is 10 000 cd/m^2 and 0.7518 is 1000. BT.2408
    // section 5: HDR reference white is 203 cd/m^2, which is PQ 0.58 and HLG
    // 0.75 on a 1000 cd/m^2 display, and SDR's 100 % maps to it.
    CHECK(pq_to_nits(1.0) == Approx(10000.0).epsilon(1e-6));
    CHECK(nits_to_pq(1000.0) == Approx(0.7518).margin(5e-4));
    CHECK(nits_to_pq(203.0) == Approx(0.58).margin(5e-3));
    CHECK(pq_to_nits(nits_to_pq(203.0)) == Approx(203.0).epsilon(1e-9));
    CHECK(hlg_to_nits(Rgb{0.75, 0.75, 0.75}, 1000.0).g == Approx(203.0).margin(2.0));
    CHECK(hlg_to_nits(Rgb{1.0, 1.0, 1.0}, 1000.0).g == Approx(1000.0).margin(1.0));

    // BT.2390's EETF from a 1000-nit grade to a 203-nit target: identity below
    // the knee, monotonic through it, the target reached exactly at the
    // source's peak and never exceeded above it, and reference white kept
    // above four fifths of the target -- the number a viewer sees.
    const double source = 1000.0;
    const double target = 203.0;
    const double knee = pq_to_nits((1.5 * nits_to_pq(target) / nits_to_pq(source) - 0.5) *
                                   nits_to_pq(source));
    CHECK(knee > 50.0);
    CHECK(knee < 120.0);
    CHECK(tone_map_bt2390(knee * 0.5, source, target) == Approx(knee * 0.5).epsilon(1e-9));
    CHECK(tone_map_bt2390(source, source, target) == Approx(target).epsilon(1e-6));
    CHECK(tone_map_bt2390(4000.0, source, target) == Approx(target).epsilon(1e-6));
    double previous = 0.0;
    for (double nits = 0.0; nits <= 1200.0; nits += 1.0) {
        const double mapped = tone_map_bt2390(nits, source, target);
        CHECK(mapped >= previous - 1e-9);
        CHECK(mapped <= target + 1e-9);
        previous = mapped;
    }
    // Reference white lands at 159 of 203 nits -- 0.78 of the display's white,
    // with the top fifth kept for the highlights above it.
    CHECK(tone_map_bt2390(203.0, source, target) > 0.75 * target);
    CHECK(tone_map_bt2390(203.0, source, target) < 0.8 * target);
    // A target the source fits in is left alone.
    CHECK(tone_map_bt2390(500.0, 600.0, 1000.0) == Approx(500.0).epsilon(1e-9));

    // The un-normalised form this replaced, for the record: against an 80-nit
    // target it started rolling off at about four nits, and a 1000-nit grade's
    // reference white came out at 52 -- two thirds of the display, with the
    // midtones lifted towards it and the highlights crushed into what was left.
    const double old_ks = 1.5 * nits_to_pq(80.0) - 0.5;
    CHECK(pq_to_nits(old_ks) < 5.0);
    const double old_white = pq_to_nits(eetf_bt2390(nits_to_pq(203.0), 1.0, nits_to_pq(80.0)));
    CHECK(old_white > 50.0);
    CHECK(old_white < 55.0);

    // The ratio form keeps hue: a colour comes out as the same three ratios.
    const Rgb colour{900.0, 300.0, 100.0};
    const Rgb mapped = tone_map_rgb(colour, source, target);
    CHECK(mapped.g / mapped.r == Approx(colour.g / colour.r).epsilon(1e-9));
    CHECK(mapped.b / mapped.r == Approx(colour.b / colour.r).epsilon(1e-9));
    CHECK(mapped.r == Approx(tone_map_bt2390(900.0, source, target)).epsilon(1e-9));
}

TEST_CASE("a colour rolls off in ratio through the real shader, on the derived gamut",
          "[video][hdr]")
{
    // **Not a grey.** The ramps above cannot tell per-component mapping from
    // the ratio form, and cannot tell a gamut matrix typed by hand from one
    // derived from the primaries. This presents a saturated PQ colour and
    // holds the pixel to both: BT.2390 on the brightest component with the
    // other two in ratio, then BT.2020 to BT.709 as the chromaticities give it.
    mp::test::Module module{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    REQUIRE(module.as<MpVideoVtbl>() != nullptr);

    Presenter presenter{*module.as<MpVideoVtbl>(), 8, 8};
    REQUIRE(presenter.ok());
    REQUIRE(presenter.tonemap("shader") == MP_OK);
    REQUIRE(presenter.configure() == "");
    const double target = presenter.target_nits();
    const double source = presenter.source_nits();
    REQUIRE(target > 0.0);

    // The matrix the module carries, checked against one derived here from
    // the two sets of primaries and D65 -- so a digit wrong in either is a
    // disagreement rather than two copies of one mistake.
    const auto derived = bt2020_to_bt709_derived();
    const Rgb red = bt2020_to_bt709(Rgb{1.0, 0.0, 0.0});
    const Rgb green = bt2020_to_bt709(Rgb{0.0, 1.0, 0.0});
    const Rgb blue = bt2020_to_bt709(Rgb{0.0, 0.0, 1.0});
    CHECK(red.r == Approx(derived[0][0]).margin(2e-3));
    CHECK(green.r == Approx(derived[0][1]).margin(2e-3));
    CHECK(blue.r == Approx(derived[0][2]).margin(2e-3));
    CHECK(red.g == Approx(derived[1][0]).margin(2e-3));
    CHECK(green.g == Approx(derived[1][1]).margin(2e-3));
    CHECK(blue.g == Approx(derived[1][2]).margin(2e-3));
    CHECK(red.b == Approx(derived[2][0]).margin(2e-3));
    CHECK(green.b == Approx(derived[2][1]).margin(2e-3));
    CHECK(blue.b == Approx(derived[2][2]).margin(2e-3));

    // A bright, saturated orange in PQ: past the knee on its red, well below
    // it on its blue. `present` takes the bytes in the order BGRA8 stores them.
    const std::uint8_t r = 200;
    const std::uint8_t g = 140;
    const std::uint8_t b = 80;
    REQUIRE(presenter.present(b, g, r) == MP_OK);
    const Rgb got = presenter.pixel();
    const Rgb nits{pq_to_nits(r / 255.0), pq_to_nits(g / 255.0), pq_to_nits(b / 255.0)};
    const Rgb mapped = tone_map_rgb(nits, source, target);
    const Rgb want =
        bt2020_to_bt709(Rgb{mapped.r / target, mapped.g / target, mapped.b / target});
    close_enough(got.r, want.r);
    close_enough(got.g, want.g);
    close_enough(got.b, want.b);
    // And it is not the per-component answer, which the ramps could not see.
    const Rgb per_component{tone_map_bt2390(nits.r, source, target),
                            tone_map_bt2390(nits.g, source, target),
                            tone_map_bt2390(nits.b, source, target)};
    CHECK(per_component.g / per_component.r != Approx(nits.g / nits.r).epsilon(1e-3));
    CHECK(mapped.g / mapped.r == Approx(nits.g / nits.r).epsilon(1e-9));
}

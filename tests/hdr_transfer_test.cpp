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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

double tone_map_bt2390(double nits, double peak)
{
    const double e = nits_to_pq(nits);
    const double max_pq = nits_to_pq(peak);
    const double ks = 1.5 * max_pq - 0.5;
    if (e < ks) {
        return nits;
    }
    const double t = (e - ks) / (1.0 - ks);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double knee = (2.0 * t3 - 3.0 * t2 + 1.0) * ks +
                        (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * max_pq;
    return pq_to_nits(knee);
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
    const double target = presenter.white_nits();
    REQUIRE(target > 0.0);

    // A ramp, and every step of it against the standard.
    for (const std::uint8_t code : {std::uint8_t{0}, std::uint8_t{32}, std::uint8_t{64},
                                    std::uint8_t{128}, std::uint8_t{192},
                                    std::uint8_t{255}}) {
        REQUIRE(presenter.present(code, code, code) == MP_OK);
        const Rgb got = presenter.pixel();

        const double nits = tone_map_bt2390(pq_to_nits(code / 255.0), target);
        // scRGB's unit is 80 nits, and BT.2020 to BT.709 for a grey is not
        // identity: the matrix rows do not each sum to one.
        const Rgb want = bt2020_to_bt709(Rgb{nits / 80.0, nits / 80.0, nits / 80.0});

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
    const double target = presenter.white_nits();
    REQUIRE(peak > 0.0);
    REQUIRE(target > 0.0);

    for (const std::uint8_t code :
         {std::uint8_t{64}, std::uint8_t{128}, std::uint8_t{192}, std::uint8_t{255}}) {
        REQUIRE(presenter.present(code, code, code) == MP_OK);
        const Rgb got = presenter.pixel();

        const double e = code / 255.0;
        const Rgb light = hlg_to_nits(Rgb{e, e, e}, peak);
        const Rgb nits{tone_map_bt2390(light.r, target), tone_map_bt2390(light.g, target),
                       tone_map_bt2390(light.b, target)};
        const Rgb want =
            bt2020_to_bt709(Rgb{nits.r / 80.0, nits.g / 80.0, nits.b / 80.0});

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

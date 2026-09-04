// SPDX-License-Identifier: GPL-3.0-or-later
//
// The presenter, rendered and read back rather than looked at.
//
// **A renderer nobody can test is a renderer judged by whether it looks
// plausible**, and §9.2 is the record of where that leads: Windows maps PQ to a
// 2.4 gamma instead of the sRGB curve, Intel published a note saying so, and it
// has survived years because the result looks fine. This tree does not get to
// make that mistake quietly.
//
// So `MpVideoVtbl::open` takes NULL for a window and renders to a texture on
// the same path a display would get, `read_back` hands the pixels over, and the
// tests below are about pixels. WARP is asked for by name because it is
// deterministic -- the same bytes on a machine with a GPU, a machine without
// one, and a CI runner.

#include <mediaperch/module.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/// The module, loaded the way the engine loads it.
struct Module {
    Module()
    {
        auto* dll = ::LoadLibraryA(MEDIAPERCH_VIDEO_D3D11);
        if (dll == nullptr) {
            return;
        }
        library = dll;
        using Entry = const MpModuleDesc*(MP_CALL*)(std::uint32_t);
        auto* entry = reinterpret_cast<Entry>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "mp_module_entry")));
        if (entry == nullptr) {
            return;
        }
        const MpModuleDesc* desc = entry(MP_ABI_VERSION);
        if (desc == nullptr || desc->kind != MP_KIND_VIDEO) {
            return;
        }
        vtbl = static_cast<const MpVideoVtbl*>(desc->vtbl);
    }
    ~Module()
    {
        if (library != nullptr) {
            ::FreeLibrary(static_cast<HMODULE>(library));
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* library = nullptr;
    const MpVideoVtbl* vtbl = nullptr;
};

/// The scRGB target is IEEE half, and DirectXMath is a dependency these tests
/// do not otherwise want for one conversion.
float half_to_float(std::uint16_t h)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits = sign;
    if (exponent == 31) {
        bits |= 0x7F800000u | (mantissa << 13);
    } else if (exponent != 0) {
        bits |= ((exponent + 112u) << 23) | (mantissa << 13);
    } else if (mantissa != 0) {
        // **Subnormals, rather than flushed to zero.** They are half's deepest
        // shadows -- below about 6e-5 -- and flushing them here would make the
        // measuring instrument blind in exactly the range where banding lives
        // and where the transfer function argument is decided.
        float value = static_cast<float>(mantissa) * 5.960464477539063e-8f;
        if (sign != 0) {
            value = -value;
        }
        return value;
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// sRGB, as the standard states it, so the test computes the expected answer
/// rather than comparing against what the shader happened to produce.
float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// One presenter, off-screen and on WARP, which is the reproducible pair.
class Presenter {
public:
    Presenter(const MpVideoVtbl& vtbl, std::uint32_t width, std::uint32_t height)
        : vtbl_(&vtbl)
    {
        if (vtbl_->open(nullptr, &handle_) != MP_OK) {
            handle_ = nullptr;
            return;
        }
        // Deterministic by choice: the software rasteriser gives the same
        // bytes on every machine, which is what makes a hash a comparison
        // rather than a note about this laptop.
        ok_ = vtbl_->set(handle_, "device", "warp") == MP_OK;

        info_ = MpVideoInfo{};
        info_.size = sizeof(info_);
        info_.width = width;
        info_.height = height;
        info_.display_width = width;
        info_.display_height = height;
        info_.primaries = 1;  // BT.709
        info_.transfer = 1;   // BT.709
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
    [[nodiscard]] MpVideo* handle() const noexcept { return handle_; }
    MpVideoInfo& info() noexcept { return info_; }

    /// Empty when it worked; otherwise what went wrong, so a failure in the
    /// test output says why rather than printing a number.
    [[nodiscard]] std::string configure()
    {
        const MpResult r = vtbl_->configure(handle_, &info_);
        if (r == MP_OK) {
            return {};
        }
        return "MpResult " + std::to_string(static_cast<unsigned>(r)) + ": " +
               described("trouble");
    }

    [[nodiscard]] MpResult present(const std::vector<std::uint8_t>& bgra,
                                   std::uint32_t width, std::uint32_t height)
    {
        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.format = MP_PIXEL_BGRA8;
        frame.width = width;
        frame.height = height;
        frame.plane[0] = bgra.data();
        frame.stride[0] = width * 4u;
        frame.pts = 0;
        return vtbl_->present(handle_, &frame);
    }

    /// The rendered pixels, as linear floats whichever width they were stored
    /// in.
    ///
    /// **Single precision off-screen by default**, because a measurement that
    /// is quantised before it is taken is measuring the quantiser. The FP16
    /// path is what a display gets and is asked for by name, so the difference
    /// between the two is itself something a test can look at.
    [[nodiscard]] std::vector<float> read_back()
    {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        MpPixelFormat format = MP_PIXEL_NONE;
        // The same grow-and-ask-again shape read_packet has: nothing is lost
        // by asking with no room first.
        const MpResult sized = vtbl_->read_back(handle_, nullptr, 0, &w, &h, &format);
        if (sized != MP_ERR_NO_MEMORY || w == 0 || h == 0) {
            return {};
        }
        const std::size_t pixels = static_cast<std::size_t>(w) * h * 4u;
        std::vector<float> out(pixels);

        if (format == MP_PIXEL_RGBA32F) {
            if (vtbl_->read_back(handle_, out.data(), out.size() * sizeof(float), &w, &h,
                                 &format) != MP_OK) {
                return {};
            }
            return out;
        }
        if (format != MP_PIXEL_RGBA16F) {
            return {};
        }
        std::vector<std::uint16_t> halves(pixels);
        if (vtbl_->read_back(handle_, halves.data(), halves.size() * sizeof(std::uint16_t),
                             &w, &h, &format) != MP_OK) {
            return {};
        }
        for (std::size_t i = 0; i < pixels; ++i) {
            out[i] = half_to_float(halves[i]);
        }
        return out;
    }

    /// One `key\tvalue\tdescription` line, by key.
    [[nodiscard]] std::string described(const char* key)
    {
        char line[512];
        for (std::uint32_t i = 0; i < 64; ++i) {
            line[0] = '\0';
            if (vtbl_->describe(handle_, i, line, sizeof(line)) != MP_OK) {
                break;
            }
            const char* tab = std::strchr(line, '\t');
            if (tab == nullptr) {
                continue;
            }
            if (std::strncmp(line, key, static_cast<std::size_t>(tab - line)) == 0 &&
                std::strlen(key) == static_cast<std::size_t>(tab - line)) {
                const char* end = std::strchr(tab + 1, '\t');
                return std::string(tab + 1, end != nullptr ? end : tab + 1 + std::strlen(tab + 1));
            }
        }
        return {};
    }

private:
    const MpVideoVtbl* vtbl_;
    MpVideo* handle_ = nullptr;
    MpVideoInfo info_{};
    bool ok_ = false;
};

/// A flat BGRA image, so what comes out can be reasoned about one pixel at a
/// time rather than compared against a picture.
std::vector<std::uint8_t> flat(std::uint32_t width, std::uint32_t height, std::uint8_t b,
                               std::uint8_t g, std::uint8_t r)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = b;
        out[i + 1] = g;
        out[i + 2] = r;
        out[i + 3] = 255;
    }
    return out;
}

} // namespace

TEST_CASE("a presenter renders with no window, so the pixels can be checked",
          "[video][d3d11]")
{
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 64, 48};
    REQUIRE(presenter.ok());
    REQUIRE(presenter.configure() == "");

    CHECK(presenter.described("surface") == "off-screen");
    CHECK(presenter.described("device") == "warp");
    // SDR content, so §9's question does not arise and nothing maps it.
    CHECK(presenter.described("applied") == "none");
    CHECK(presenter.described("format") == "fp16 scRGB");
    CHECK(presenter.described("trouble") == "nothing");

    const std::vector<std::uint8_t> source = flat(64, 48, 0x20, 0x40, 0x80);
    REQUIRE(presenter.present(source, 64, 48) == MP_OK);

    const std::vector<float> shown = presenter.read_back();
    REQUIRE(shown.size() == 64u * 48u * 4u);

    // **What the target holds is linear scRGB**, so the expected value is the
    // sRGB decode of what went in -- computed here from the standard rather
    // than compared against whatever the shader produced last time.
    const float expect_r = srgb_to_linear(0x80 / 255.0f);
    const float expect_g = srgb_to_linear(0x40 / 255.0f);
    const float expect_b = srgb_to_linear(0x20 / 255.0f);
    // **Single precision, so the margin is the shader arithmetic and nothing
    // else.** A tolerance of 1e-3 would pass on a pipeline that quantised to
    // eight bits somewhere; 1e-6 is what a correct FP32 path actually gives.
    for (std::size_t i = 0; i < shown.size(); i += 4) {
        CHECK(shown[i + 0] == Catch::Approx(expect_r).margin(1e-6));
        CHECK(shown[i + 1] == Catch::Approx(expect_g).margin(1e-6));
        CHECK(shown[i + 2] == Catch::Approx(expect_b).margin(1e-6));
        CHECK(shown[i + 3] == Catch::Approx(1.0f).margin(1e-6));
    }
}

TEST_CASE("WARP renders the same bytes every time", "[video][d3d11]")
{
    // What makes any of this a measurement. Two presenters, two devices, two
    // renders of the same frame -- and if they differ there is nothing to
    // compare a colour pipeline against on anybody else's machine.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    const std::vector<std::uint8_t> source = flat(32, 32, 0x11, 0x99, 0xEE);
    std::vector<float> first;
    std::vector<float> second;

    for (std::vector<float>* into : {&first, &second}) {
        Presenter presenter{*module.vtbl, 32, 32};
        REQUIRE(presenter.ok());
        REQUIRE(presenter.configure() == "");
        REQUIRE(presenter.present(source, 32, 32) == MP_OK);
        *into = presenter.read_back();
        REQUIRE(!into->empty());
    }
    CHECK(first == second);
}

TEST_CASE("the sRGB decode is the piecewise curve, not a 2.2 power",
          "[video][d3d11][colour]")
{
    // §9.2 is about Windows using a 2.4 gamma where the sRGB curve belongs, and
    // the difference lives in the dark end -- which is exactly where the
    // piecewise segment is and where a power approximation is worst. So the
    // check is on a dark grey, and it is against the standard rather than
    // against what came out last time.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 16, 16};
    REQUIRE(presenter.ok());
    REQUIRE(presenter.configure() == "");

    for (const std::uint8_t level : {std::uint8_t{1}, std::uint8_t{4}, std::uint8_t{10},
                                     std::uint8_t{64}, std::uint8_t{200}}) {
        const std::vector<std::uint8_t> source = flat(16, 16, level, level, level);
        REQUIRE(presenter.present(source, 16, 16) == MP_OK);
        const std::vector<float> shown = presenter.read_back();
        REQUIRE(shown.size() == 16u * 16u * 4u);

        const float encoded = level / 255.0f;
        const float expected = srgb_to_linear(encoded);
        CHECK(shown[0] == Catch::Approx(expected).margin(1e-6));

        // **And it is not a 2.2 power**, which is the approximation §9.2
        // records Windows using where the sRGB curve belongs. At a level of 1
        // the two answers differ by a factor of nine -- invisible after an
        // 8-bit round trip, which is exactly why this test reads linear floats
        // and why the first version of read_back could not have caught it.
        const float wrong = std::pow(encoded, 2.2f);
        if (level <= 10) {
            CHECK(std::abs(shown[0] - wrong) > std::abs(shown[0] - expected));
        }
    }
}

TEST_CASE("a presenter refuses what it cannot do, and says so", "[video][d3d11]")
{
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 16, 16};
    REQUIRE(presenter.ok());
    REQUIRE(presenter.configure() == "");

    // NV12 is what every hardware decoder produces and nothing here decodes
    // video yet, so the path has no producer and no test could check it. It is
    // refused with a sentence rather than accepted and rendered as noise.
    std::vector<std::uint8_t> planes(16u * 16u * 3u / 2u, 0x80);
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.format = MP_PIXEL_NV12;
    frame.width = 16;
    frame.height = 16;
    frame.plane[0] = planes.data();
    frame.stride[0] = 16;
    frame.plane[1] = planes.data() + 16u * 16u;
    frame.stride[1] = 16;
    CHECK(module.vtbl->present(presenter.handle(), &frame) == MP_ERR_UNSUPPORTED);
    CHECK(presenter.described("trouble").find("NV12") != std::string::npos);
}

TEST_CASE("what a person can set, and what it reports back", "[video][d3d11]")
{
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 16, 16};
    REQUIRE(presenter.ok());

    CHECK(module.vtbl->set(presenter.handle(), "tonemap", "shader") == MP_OK);
    CHECK(module.vtbl->set(presenter.handle(), "tonemap", "reinhard") == MP_ERR_INVALID);
    CHECK(module.vtbl->set(presenter.handle(), "composited", "0") == MP_OK);
    CHECK(module.vtbl->set(presenter.handle(), "nonsense", "1") == MP_ERR_UNSUPPORTED);
    CHECK(presenter.described("tonemap") == "shader");

    REQUIRE(presenter.configure() == "");
    // The device is made by `configure`, so asking for a different one
    // afterwards is refused rather than silently ignored.
    CHECK(module.vtbl->set(presenter.handle(), "device", "hardware") == MP_ERR_UNSUPPORTED);

    // The stream is SDR, so the tone mapper a person asked for is not in the
    // path -- and `applied` says what is actually happening rather than what
    // was requested, which is the difference `describe` exists to carry.
    CHECK(presenter.described("applied") == "none");
}

TEST_CASE("what presenting in FP16 costs, measured rather than assumed",
          "[video][d3d11][colour]")
{
    // **DXGI will not present anything wider.** A flip-model swap chain takes
    // 8-bit UNORM, 10-bit UNORM or R16G16B16A16_FLOAT and nothing else, so the
    // half is the platform's ceiling rather than this module's choice -- and
    // the honest thing is to know what it costs instead of assuming it is
    // nothing.
    //
    // Half is *relatively* precise, which suits a linear light buffer: about
    // eleven significant bits everywhere, so the spacing is fine in the
    // shadows where banding lives and coarsest near white where the eye is
    // least able to see a step.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    const std::vector<std::uint8_t> source = flat(16, 16, 0x08, 0x80, 0xFF);

    std::vector<float> wide;
    std::vector<float> presented;
    for (int pass = 0; pass < 2; ++pass) {
        Presenter presenter{*module.vtbl, 16, 16};
        REQUIRE(presenter.ok());
        REQUIRE(module.vtbl->set(presenter.handle(), "precision",
                                 pass == 0 ? "fp32" : "fp16") == MP_OK);
        REQUIRE(presenter.configure() == "");
        CHECK(presenter.described("precision") == (pass == 0 ? "fp32" : "fp16"));
        REQUIRE(presenter.present(source, 16, 16) == MP_OK);
        (pass == 0 ? wide : presented) = presenter.read_back();
    }
    REQUIRE(wide.size() == 16u * 16u * 4u);
    REQUIRE(presented.size() == wide.size());

    // The two differ, and by no more than half's own spacing. If they were
    // identical the wide target would not be wide; if they differed by more,
    // something other than the store would be rounding.
    float worst_relative = 0.0f;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const float reference = wide[i];
        if (reference <= 0.0f) {
            continue;
        }
        worst_relative =
            std::max(worst_relative, std::abs(presented[i] - reference) / reference);
    }
    // Half has ten stored mantissa bits, so one unit in the last place is at
    // most 2^-11 of the value.
    CHECK(worst_relative <= 1.0f / 2048.0f);

    // And the relative bound holds in the shadows as well as at white, which
    // is the property an integer format does not have: 8-bit at this level
    // would be a relative error of several per cent.
    const float dark = wide[2]; // the 0x08 channel, decoded to about 0.0022
    REQUIRE(dark > 0.0f);
    CHECK(dark < 0.01f);
    CHECK(std::abs(presented[2] - dark) / dark <= 1.0f / 2048.0f);
}

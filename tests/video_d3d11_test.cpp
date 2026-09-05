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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

namespace {

/// The layouts these tests hand over, spelled by the ABI's own macros so that
/// a test states the same thing a decoder would.
constexpr MpPixelLayout k_bgra8 = MP_LAYOUT_BGRA8;
constexpr MpPixelLayout k_nv12 = MP_LAYOUT_NV12;

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
        const MpModuleDesc* found = entry(MP_ABI_VERSION);
        if (found == nullptr || found->kind != MP_KIND_VIDEO) {
            return;
        }
        desc = found;
        vtbl = static_cast<const MpVideoVtbl*>(found->vtbl);
    }
    ~Module()
    {
        if (library != nullptr) {
            // **`shutdown` before `FreeLibrary`, because that is what a host
            // does.** `ModuleRegistry` calls `init` after loading and
            // `shutdown` before unloading; a harness that skipped the second
            // was not modelling the host, it was modelling a host with a bug.
            // `codec_mft` had to stop Media Foundation from a static
            // destructor because nothing here called its shutdown, and doing
            // that during `FreeLibrary` deadlocks against the loader lock.
            if (desc != nullptr && desc->shutdown != nullptr) {
                desc->shutdown();
            }
            ::FreeLibrary(static_cast<HMODULE>(library));
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* library = nullptr;
    /// Kept so the destructor can call `shutdown`, which is what a host does.
    const MpModuleDesc* desc = nullptr;
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
        info_.primaries = 1; // BT.709
        // **sRGB by default here, because the source is BGRA8**: a test pattern
        // and a software decoder produce sRGB, and video tagged BT.709 gets
        // BT.1886 instead. Which curve is not a matter of taste and the code
        // point is how a stream says which -- see the shader.
        info_.transfer = 13; // IEC 61966-2-1, which is sRGB
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
        frame.layout = k_bgra8;
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
        MpPixelLayout layout{};
        layout.size = sizeof(layout);
        // The same grow-and-ask-again shape read_packet has: nothing is lost
        // by asking with no room first.
        const MpResult sized = vtbl_->read_back(handle_, nullptr, 0, &w, &h, &layout);
        if (sized != MP_ERR_NO_MEMORY || w == 0 || h == 0) {
            return {};
        }
        const std::size_t pixels = static_cast<std::size_t>(w) * h * 4u;
        std::vector<float> out(pixels);

        // Asked of the layout rather than matched against a name: single and
        // half precision differ in one field, which is the shape v4 exists to
        // have.
        if ((layout.flags & MP_PIXEL_FLOAT) == 0u) {
            return {};
        }
        if (layout.container_bits == 32u) {
            if (vtbl_->read_back(handle_, out.data(), out.size() * sizeof(float), &w, &h,
                                 &layout) != MP_OK) {
                return {};
            }
            return out;
        }
        if (layout.container_bits != 16u) {
            return {};
        }
        std::vector<std::uint16_t> halves(pixels);
        if (vtbl_->read_back(handle_, halves.data(), halves.size() * sizeof(std::uint16_t),
                             &w, &h, &layout) != MP_OK) {
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
    CHECK(presenter.described("encoding") == "linear scRGB");
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

    std::vector<std::uint8_t> planes(16u * 16u * 3u / 2u, 0x80);
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = k_nv12;
    frame.width = 16;
    frame.height = 16;
    frame.plane[0] = planes.data();
    frame.stride[0] = 16;
    frame.plane[1] = planes.data() + 16u * 16u;
    frame.stride[1] = 16;
    CHECK(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

    // A plane that is not there, and a stride too short for the width it
    // claims, are both refused rather than read past. **A `texture` that is
    // not a texture is not on that list**: the ABI takes a resource pointer and
    // a caller that passes rubbish there has done what a caller passing rubbish
    // in `plane[0]` has done, which no amount of checking here can catch.
    frame.plane[1] = nullptr;
    CHECK(module.vtbl->present(presenter.handle(), &frame) == MP_ERR_UNSUPPORTED);
    frame.plane[1] = planes.data() + 16u * 16u;
    frame.stride[0] = 4;
    CHECK(module.vtbl->present(presenter.handle(), &frame) == MP_ERR_UNSUPPORTED);
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
    // Half is *relatively* precise -- a step between 1/2048 and 1/1024 of the
    // value, everywhere -- which suits a linear light buffer and is why it
    // beats a 10-bit integer at the same width. It is **not** sufficient: a
    // 12-bit output needs 1/1706 at white, so half is already 1.7 times short
    // there and 27 times short at 16 bits. What this test does is establish
    // that the loss is exactly half's own quantisation and nothing else, so
    // that the number in plan.md §9.10 is a property of the code.
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

TEST_CASE("NV12 becomes RGB by the matrix the container named", "[video][d3d11][yuv]")
{
    // **The exact check.** A synthetic NV12 frame whose Y, Cb and Cr this test
    // chooses, against the BT.709 conversion computed here from the published
    // luma weights -- not against whatever the shader produced last time. The
    // read-back is FP32, so the comparison is the arithmetic rather than a
    // quantisation of it.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 32, 32};
    REQUIRE(presenter.ok());
    presenter.info().matrix = 1; // BT.709
    presenter.info().transfer = 1;
    presenter.info().primaries = 1;
    REQUIRE(presenter.configure() == "");

    // Studio range, which is what a decoder produces unless a container says
    // otherwise: Y in 16..235, chroma centred on 128.
    constexpr std::uint8_t k_y = 120;
    constexpr std::uint8_t k_cb = 90;
    constexpr std::uint8_t k_cr = 200;

    std::vector<std::uint8_t> luma(32u * 32u, k_y);
    std::vector<std::uint8_t> chroma(16u * 16u * 2u);
    for (std::size_t i = 0; i < chroma.size(); i += 2) {
        chroma[i] = k_cb;
        chroma[i + 1] = k_cr;
    }

    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = k_nv12;
    frame.width = 32;
    frame.height = 32;
    frame.plane[0] = luma.data();
    frame.stride[0] = 32;
    frame.plane[1] = chroma.data();
    frame.stride[1] = 32;
    REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

    const std::vector<float> shown = presenter.read_back();
    REQUIRE(shown.size() == 32u * 32u * 4u);

    // BT.709, computed here in double from Kr and Kb, which is the same
    // derivation yuv_matrix.cpp does and deliberately not the same code.
    const double kr = 0.2126;
    const double kb = 0.0722;
    const double kg = 1.0 - kr - kb;
    const double y = (k_y / 255.0 - 16.0 / 255.0) * (255.0 / 219.0);
    const double u = (k_cb / 255.0 - 0.5) * (255.0 / 224.0);
    const double v = (k_cr / 255.0 - 0.5) * (255.0 / 224.0);
    const double r = y + 2.0 * (1.0 - kr) * v;
    const double g = y - 2.0 * kb * (1.0 - kb) / kg * u - 2.0 * kr * (1.0 - kr) / kg * v;
    const double b = y + 2.0 * (1.0 - kb) * u;

    // BT.1886, a pure 2.4, because the stream is tagged BT.709 rather than
    // sRGB -- see the shader's comment on which curve and why.
    const auto to_linear = [](double c) {
        return std::pow(std::clamp(c, 0.0, 1.0), 2.4);
    };
    CHECK(shown[0] == Catch::Approx(to_linear(r)).margin(1e-5));
    CHECK(shown[1] == Catch::Approx(to_linear(g)).margin(1e-5));
    CHECK(shown[2] == Catch::Approx(to_linear(b)).margin(1e-5));
    CHECK(shown[3] == Catch::Approx(1.0f).margin(1e-6));

    // And a chosen colour is not grey, which is the cheapest evidence that the
    // matrix ran at all: a conversion that dropped the chroma would give three
    // equal channels for any Y.
    CHECK(std::abs(shown[0] - shown[2]) > 0.01f);
}

namespace {

/// A planar layout stated the way a decoder that is not here yet would state
/// it: dav1d hands back I420, I422, I444 and I400 at 8, 10 and 12 bits, with
/// the significant bits at the BOTTOM of the container rather than the top.
MpPixelLayout planar_layout(MpChroma chroma, std::uint32_t bits)
{
    MpPixelLayout out{};
    out.size = sizeof(out);
    out.chroma = chroma;
    out.packing = MP_PACK_PLANAR;
    out.bits = bits;
    out.container_bits = bits > 8u ? 16u : 8u;
    return out;
}

/// BT.709 from the published luma weights, in double, deliberately not the
/// same code `yuv_matrix.cpp` runs. `scale` puts a sample of any depth on the
/// 0..1 the standard is written in.
struct Rgb {
    double r;
    double g;
    double b;
};
Rgb bt709(double y_code, double cb_code, double cr_code, double full)
{
    const double kr = 0.2126;
    const double kb = 0.0722;
    const double kg = 1.0 - kr - kb;
    const double y = (y_code / full - 16.0 / 255.0) * (255.0 / 219.0);
    const double u = (cb_code / full - 0.5) * (255.0 / 224.0);
    const double v = (cr_code / full - 0.5) * (255.0 / 224.0);
    return {y + 2.0 * (1.0 - kr) * v,
            y - 2.0 * kb * (1.0 - kb) / kg * u - 2.0 * kr * (1.0 - kr) / kg * v,
            y + 2.0 * (1.0 - kb) * u};
}

double bt1886(double c) { return std::pow(std::clamp(c, 0.0, 1.0), 2.4); }

} // namespace

TEST_CASE("planar 4:2:0, 4:2:2 and 4:4:4 reach the same conversion",
          "[video][d3d11][yuv]")
{
    // **Three subsamplings and one answer**, which is the point of describing a
    // layout rather than naming it. The chroma plane is a different size in
    // each and nothing else changes: the shader samples with normalised
    // coordinates, so a half-width plane and a full-width one are the same
    // call, and the picture must come out identical for a flat colour.
    //
    // No decoder in this tree produces planar yet -- dav1d is what will. The
    // frames are built here, which is exactly how the presenter was checked
    // before there was anything to decode at all.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    constexpr std::uint8_t k_y = 120;
    constexpr std::uint8_t k_cb = 90;
    constexpr std::uint8_t k_cr = 200;
    const Rgb want = bt709(k_y, k_cb, k_cr, 255.0);

    const MpChroma layouts[] = {MP_CHROMA_420, MP_CHROMA_422, MP_CHROMA_444};
    for (MpChroma chroma : layouts) {
        const MpPixelLayout layout = planar_layout(chroma, 8u);
        INFO("chroma " << static_cast<unsigned>(chroma));

        Presenter presenter{*module.vtbl, 32, 32};
        REQUIRE(presenter.ok());
        presenter.info().matrix = 1; // BT.709
        presenter.info().transfer = 1;
        presenter.info().primaries = 1;
        REQUIRE(presenter.configure() == "");

        const std::uint32_t cw = mp_pixel_chroma_width(&layout, 32u);
        const std::uint32_t ch = mp_pixel_chroma_height(&layout, 32u);
        std::vector<std::uint8_t> luma(32u * 32u, k_y);
        std::vector<std::uint8_t> cb(static_cast<std::size_t>(cw) * ch, k_cb);
        std::vector<std::uint8_t> cr(static_cast<std::size_t>(cw) * ch, k_cr);

        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout = layout;
        frame.width = 32;
        frame.height = 32;
        frame.plane[0] = luma.data();
        frame.stride[0] = 32;
        frame.plane[1] = cb.data();
        frame.stride[1] = cw;
        frame.plane[2] = cr.data();
        frame.stride[2] = cw;
        REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

        const std::vector<float> shown = presenter.read_back();
        REQUIRE(shown.size() == 32u * 32u * 4u);
        CHECK(shown[0] == Catch::Approx(bt1886(want.r)).margin(1e-5));
        CHECK(shown[1] == Catch::Approx(bt1886(want.g)).margin(1e-5));
        CHECK(shown[2] == Catch::Approx(bt1886(want.b)).margin(1e-5));
        // Not grey, so a run that dropped both chroma planes would fail here
        // rather than agree with itself.
        CHECK(std::abs(shown[0] - shown[2]) > 0.01f);
    }
}

TEST_CASE("4:0:0 is grey rather than whatever an unbound texture samples to",
          "[video][d3d11][yuv]")
{
    // **The failure this guards against is a picture, not an error.** A
    // monochrome frame has one plane; sampling two textures that were never
    // made gives zero, and zero through `(c - 0.5) * chroma_scale` is a strong
    // green. So the shader is told there is no chroma rather than left to read
    // it, and grey means grey.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 16, 16};
    REQUIRE(presenter.ok());
    presenter.info().matrix = 1;
    presenter.info().transfer = 1;
    REQUIRE(presenter.configure() == "");

    constexpr std::uint8_t k_y = 150;
    std::vector<std::uint8_t> luma(16u * 16u, k_y);

    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = planar_layout(MP_CHROMA_MONO, 8u);
    frame.width = 16;
    frame.height = 16;
    frame.plane[0] = luma.data();
    frame.stride[0] = 16;
    REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

    const std::vector<float> shown = presenter.read_back();
    REQUIRE(shown.size() == 16u * 16u * 4u);

    // Chroma at its centre is chroma that says nothing, so the answer is the
    // luma channel three times.
    //
    // **And the centre is 127.5, not 128.** Half of the full scale falls
    // between two codes at every even depth, so a monochrome frame written as
    // 4:2:0 with its chroma planes filled with 128 is not quite neutral -- a
    // ten-thousandth of the range green. Saying 0 chroma means saying exactly
    // 0.5, which is what `has_chroma` does and what an integer code cannot.
    const Rgb want = bt709(k_y, 127.5, 127.5, 255.0);
    CHECK(shown[0] == Catch::Approx(bt1886(want.r)).margin(1e-5));
    CHECK(shown[0] == Catch::Approx(shown[1]).margin(1e-6));
    CHECK(shown[1] == Catch::Approx(shown[2]).margin(1e-6));
}

TEST_CASE("ten and twelve bits at the bottom of the container, not the top",
          "[video][d3d11][yuv]")
{
    // **The sixty-four the ABI's `shift` exists for, as pixels.** P010 puts ten
    // bits at the top of sixteen and dav1d puts them at the bottom; both are
    // ten-bit 4:2:0 and a presenter that assumed either would be wrong about
    // the other by a factor of sixty-four. Here the bits are at the bottom,
    // which is the case that did not exist before v4.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    struct Case {
        std::uint32_t bits;
        std::uint32_t y;
        std::uint32_t cb;
        std::uint32_t cr;
    };
    const Case cases[] = {{10u, 480u, 360u, 800u}, {12u, 1920u, 1440u, 3200u}};

    for (const Case& c : cases) {
        INFO(c.bits << " bits");
        const MpPixelLayout layout = planar_layout(MP_CHROMA_444, c.bits);
        CHECK(layout.container_bits == 16u);
        CHECK(layout.shift == 0u);

        Presenter presenter{*module.vtbl, 16, 16};
        REQUIRE(presenter.ok());
        presenter.info().matrix = 1;
        presenter.info().transfer = 1;
        REQUIRE(presenter.configure() == "");

        std::vector<std::uint16_t> luma(16u * 16u, static_cast<std::uint16_t>(c.y));
        std::vector<std::uint16_t> cb(16u * 16u, static_cast<std::uint16_t>(c.cb));
        std::vector<std::uint16_t> cr(16u * 16u, static_cast<std::uint16_t>(c.cr));

        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout = layout;
        frame.width = 16;
        frame.height = 16;
        frame.plane[0] = luma.data();
        frame.stride[0] = 32;
        frame.plane[1] = cb.data();
        frame.stride[1] = 32;
        frame.plane[2] = cr.data();
        frame.stride[2] = 32;
        REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

        const std::vector<float> shown = presenter.read_back();
        REQUIRE(shown.size() == 16u * 16u * 4u);

        // The code values are read against the full scale of their own depth,
        // which is what `sample_scale` restores after a container-normalised
        // sample.
        const double full = static_cast<double>((1u << c.bits) - 1u);
        const Rgb want = bt709(c.y, c.cb, c.cr, full);
        CHECK(shown[0] == Catch::Approx(bt1886(want.r)).margin(2e-5));
        CHECK(shown[1] == Catch::Approx(bt1886(want.g)).margin(2e-5));
        CHECK(shown[2] == Catch::Approx(bt1886(want.b)).margin(2e-5));
    }
}

TEST_CASE("studio range and full range are not the same picture", "[video][d3d11][yuv]")
{
    // Treating 16..235 as 0..255 crushes the blacks and clips the whites, which
    // reads as a contrast setting rather than as a bug -- so the flag the
    // container carries has to reach the shader, and this is the test that it
    // does.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    std::vector<float> studio;
    std::vector<float> full;
    for (int pass = 0; pass < 2; ++pass) {
        Presenter presenter{*module.vtbl, 16, 16};
        REQUIRE(presenter.ok());
        presenter.info().matrix = 1;
        presenter.info().transfer = 1;
        if (pass == 1) {
            presenter.info().flags |= MP_VIDEO_FULL_RANGE;
        }
        REQUIRE(presenter.configure() == "");

        std::vector<std::uint8_t> luma(16u * 16u, 235);
        std::vector<std::uint8_t> chroma(8u * 8u * 2u, 128);
        MpVideoFrame frame{};
        frame.size = sizeof(frame);
        frame.layout = k_nv12;
        frame.width = 16;
        frame.height = 16;
        frame.plane[0] = luma.data();
        frame.stride[0] = 16;
        frame.plane[1] = chroma.data();
        frame.stride[1] = 16;
        REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);
        (pass == 0 ? studio : full) = presenter.read_back();
    }
    REQUIRE(!studio.empty());
    REQUIRE(!full.empty());

    // 235 is studio white, so studio range makes it 1.0 and full range makes it
    // 235/255 -- which after a 2.4 power is about 0.83.
    CHECK(studio[0] == Catch::Approx(1.0f).margin(1e-4));
    CHECK(full[0] < 0.9f);
    CHECK(full[0] > 0.7f);
}

TEST_CASE("video tagged BT.709 is decoded with BT.1886, not with sRGB",
          "[video][d3d11][colour]")
{
    // **Two curves, and which one is not a matter of taste.** BT.709 states a
    // camera OETF and its reference display EOTF is BT.1886, a pure 2.4 --
    // which is what the picture was graded on. Decoding it with the sRGB curve
    // lifts the shadows, which is the mirror image of the fault §9.2 records
    // Windows committing in the other direction, and it is invisible until
    // somebody puts two players side by side.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    const std::vector<std::uint8_t> source = flat(16, 16, 0x80, 0x80, 0x80);
    const float encoded = 0x80 / 255.0f;

    std::vector<float> as_srgb;
    std::vector<float> as_bt1886;
    for (int pass = 0; pass < 2; ++pass) {
        Presenter presenter{*module.vtbl, 16, 16};
        REQUIRE(presenter.ok());
        presenter.info().transfer = pass == 0 ? 13u : 1u;
        REQUIRE(presenter.configure() == "");
        REQUIRE(presenter.present(source, 16, 16) == MP_OK);
        (pass == 0 ? as_srgb : as_bt1886) = presenter.read_back();
    }
    REQUIRE(!as_srgb.empty());
    REQUIRE(!as_bt1886.empty());

    CHECK(as_srgb[0] == Catch::Approx(srgb_to_linear(encoded)).margin(1e-6));
    CHECK(as_bt1886[0] == Catch::Approx(std::pow(encoded, 2.4f)).margin(1e-6));

    // Twelve per cent apart at middle grey, which is the size of the mistake.
    CHECK(as_srgb[0] > as_bt1886[0] * 1.1f);
}

TEST_CASE("a decoder's own texture is viewed rather than copied", "[video][d3d11][yuv]")
{
    // **What decoding on the GPU is for**, tested without a hardware decoder.
    // An ID3D11VideoDecoder writes NV12 into an array it owns and a frame is a
    // slice of it; this makes such a texture by hand on the presenter's own
    // device -- which is what `get_device` exists to hand over -- and checks
    // the presenter samples it in place. Media Foundation granting the binding
    // is a separate question and a separate test: this one asks for WARP,
    // which has no video device at all, and codec_mft_test.cpp takes the
    // hardware one.
    Module module;
    REQUIRE(module.vtbl != nullptr);

    Presenter presenter{*module.vtbl, 32, 32};
    REQUIRE(presenter.ok());
    presenter.info().matrix = 1;  // BT.709
    presenter.info().transfer = 1;
    REQUIRE(presenter.configure() == "");

    MpGraphicsDevice graphics{};
    graphics.size = sizeof(graphics);
    REQUIRE(module.vtbl->get_device(presenter.handle(), &graphics) == MP_OK);
    CHECK(graphics.api == MP_GRAPHICS_D3D11);
    REQUIRE(graphics.device != nullptr);
    auto* device = static_cast<ID3D11Device*>(graphics.device);

    // NV12 with a shader-resource binding, which is exactly what codec_mft asks
    // an MFT for. If WARP will not give it, that is worth failing over rather
    // than skipping: a test that quietly does nothing is also a claim.
    UINT support = 0;
    REQUIRE(SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)));
    REQUIRE((support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0);

    constexpr std::uint8_t k_y = 120;
    constexpr std::uint8_t k_cb = 90;
    constexpr std::uint8_t k_cr = 200;
    std::vector<std::uint8_t> nv12(32u * 32u + 16u * 32u);
    std::fill(nv12.begin(), nv12.begin() + 32 * 32, k_y);
    for (std::size_t i = 32u * 32u; i < nv12.size(); i += 2) {
        nv12[i] = k_cb;
        nv12[i + 1] = k_cr;
    }

    // An array of two slices, so the frame's index is exercised rather than
    // assumed to be zero -- a decoder hands out one array and an index into it.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA seed[2]{};
    std::vector<std::uint8_t> blank(nv12.size(), 0);
    seed[0].pSysMem = blank.data();
    seed[0].SysMemPitch = 32;
    seed[1].pSysMem = nv12.data();
    seed[1].SysMemPitch = 32;

    ID3D11Texture2D* texture = nullptr;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, seed, &texture)));

    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = k_nv12;
    frame.width = 32;
    frame.height = 32;
    frame.texture = texture;
    frame.texture_index = 1; // the slice that has the picture in it
    REQUIRE(module.vtbl->present(presenter.handle(), &frame) == MP_OK);

    const std::vector<float> shown = presenter.read_back();
    REQUIRE(shown.size() == 32u * 32u * 4u);

    // The same conversion the CPU-plane test checks, so the two paths are held
    // to one answer rather than each to its own.
    const double kr = 0.2126;
    const double kb = 0.0722;
    const double kg = 1.0 - kr - kb;
    const double y = (k_y / 255.0 - 16.0 / 255.0) * (255.0 / 219.0);
    const double u = (k_cb / 255.0 - 0.5) * (255.0 / 224.0);
    const double v = (k_cr / 255.0 - 0.5) * (255.0 / 224.0);
    const auto to_linear = [](double c) { return std::pow(std::clamp(c, 0.0, 1.0), 2.4); };
    CHECK(shown[0] == Catch::Approx(to_linear(y + 2.0 * (1.0 - kr) * v)).margin(1e-5));
    CHECK(shown[2] == Catch::Approx(to_linear(y + 2.0 * (1.0 - kb) * u)).margin(1e-5));
    // Green is worth checking on its own: red and blue take one chroma each,
    // and green is the only channel where both coefficients are derived rather
    // than read off, so a mistake in the luma weights shows here first.
    CHECK(shown[1] == Catch::Approx(to_linear(y - 2.0 * kb * (1.0 - kb) / kg * u -
                                              2.0 * kr * (1.0 - kr) / kg * v))
                          .margin(1e-5));

    // And a slice past the end of the array is refused rather than read.
    frame.texture_index = 7;
    CHECK(module.vtbl->present(presenter.handle(), &frame) == MP_ERR_UNSUPPORTED);
    CHECK(presenter.described("trouble").find("slice") != std::string::npos);
    frame.texture_index = 1;

    // **A texture without the binding is the case MF_SA_D3D11_BINDFLAGS exists
    // to prevent**, and it is a driver saying no rather than a caller making a
    // mistake -- so it is refused with a sentence naming the flag rather than
    // copied silently into something that would work.
    D3D11_TEXTURE2D_DESC plain = desc;
    plain.ArraySize = 1;
    plain.BindFlags = 0;
    plain.Usage = D3D11_USAGE_STAGING;
    plain.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* unbindable = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&plain, nullptr, &unbindable))) {
        MpVideoFrame unusable = frame;
        unusable.texture = unbindable;
        unusable.texture_index = 0;
        CHECK(module.vtbl->present(presenter.handle(), &unusable) == MP_ERR_UNSUPPORTED);
        CHECK(presenter.described("trouble").find("D3D11_BIND_SHADER_RESOURCE") !=
              std::string::npos);
        unbindable->Release();
    }

    texture->Release();
}

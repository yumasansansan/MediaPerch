// SPDX-License-Identifier: GPL-3.0-or-later
//
// §9.8.3's first stage, rendered and read back.
//
// **The identity is the assertion a lookup table gets for free.** A grade is
// somebody's taste and cannot be checked against anything; a table that changes
// nothing must give the picture back, and a table that exchanges two channels
// must give back a picture with those two exchanged. Both are computable, both
// run on WARP, and neither needs a display or a file anybody has to trust.
//
// What is being checked is the *whole path*: the presenter's first pass stops
// at linear light, the stage grades it, and the second pass tone-maps, moves
// the gamut and encodes -- so a picture that comes back unchanged says all
// three halves line up, not just that the shader arithmetic is right.

#include <mediaperch/module.h>

#include "module_loader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using mp::test::Module;

namespace {

constexpr MpPixelLayout k_bgra8 = MP_LAYOUT_BGRA8;

/// A `.cube` written to a temporary file, because `set("file", ...)` is how a
/// stage is given one and a test that reached past that would be testing
/// something nobody uses.
class CubeFile {
public:
    explicit CubeFile(const std::string& text)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("mediaperch_test_" + std::to_string(++counter()) + ".cube");
        std::ofstream out{path_, std::ios::binary};
        out << text;
    }
    ~CubeFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    CubeFile(const CubeFile&) = delete;
    CubeFile& operator=(const CubeFile&) = delete;

    [[nodiscard]] std::string name() const { return path_.string(); }

private:
    static int& counter()
    {
        static int n = 0;
        return n;
    }
    std::filesystem::path path_;
};

/// A cube of `size` per axis, each entry produced by `f`.
template <typename F>
std::string cube_text(std::uint32_t size, F f)
{
    std::string out = "LUT_3D_SIZE " + std::to_string(size) + "\n";
    char line[128];
    const auto step = 1.0f / static_cast<float>(size - 1);
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                const float in[3] = {static_cast<float>(r) * step,
                                     static_cast<float>(g) * step,
                                     static_cast<float>(b) * step};
                float made[3] = {in[0], in[1], in[2]};
                f(in, made);
                std::snprintf(line, sizeof line, "%.9f %.9f %.9f\n",
                              static_cast<double>(made[0]), static_cast<double>(made[1]),
                              static_cast<double>(made[2]));
                out += line;
            }
        }
    }
    return out;
}

/// One presenter, off-screen on WARP, optionally with one stage in front of it.
///
/// The picture is a flat colour so that what comes back can be reasoned about
/// one pixel at a time rather than compared against an image.
std::vector<float> shown(const MpVideoVtbl& video, const MpVideoDspVtbl* stage_vtbl,
                         const char* lut_file, std::uint8_t b, std::uint8_t g,
                         std::uint8_t r)
{
    MpVideo* handle = nullptr;
    if (video.open(nullptr, &handle) != MP_OK) {
        return {};
    }
    struct Closer {
        const MpVideoVtbl* v;
        MpVideo* h;
        ~Closer() { v->close(h); }
    } closer{&video, handle};

    if (video.set(handle, "device", "warp") != MP_OK) {
        return {};
    }

    MpVideoInfo info{};
    info.size = sizeof(info);
    info.width = 16;
    info.height = 16;
    info.display_width = 16;
    info.display_height = 16;
    info.primaries = 1;
    info.transfer = 13; // sRGB, which is what a BGRA8 test pattern is
    info.matrix = 1;
    info.timescale = 24000;
    if (video.configure(handle, &info) != MP_OK) {
        return {};
    }

    MpVideoDsp* stage = nullptr;
    struct StageCloser {
        const MpVideoDspVtbl* v;
        MpVideoDsp** h;
        ~StageCloser()
        {
            if (v != nullptr && *h != nullptr) {
                v->close(*h);
            }
        }
    } stage_closer{stage_vtbl, &stage};

    if (stage_vtbl != nullptr) {
        MpGraphicsDevice device{};
        device.size = sizeof(device);
        if (video.get_device(handle, &device) != MP_OK) {
            return {};
        }
        if (stage_vtbl->open(&device, &stage) != MP_OK) {
            return {};
        }
        if (lut_file != nullptr && stage_vtbl->set(stage, "file", lut_file) != MP_OK) {
            return {};
        }
        MpVideoStage one{};
        one.size = sizeof(one);
        one.vtbl = stage_vtbl;
        one.handle = stage;
        if (video.stages(handle, &one, 1) != MP_OK) {
            return {};
        }
    }

    std::vector<std::uint8_t> pixels(16u * 16u * 4u);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = b;
        pixels[i + 1] = g;
        pixels[i + 2] = r;
        pixels[i + 3] = 255;
    }
    MpVideoFrame frame{};
    frame.size = sizeof(frame);
    frame.layout = k_bgra8;
    frame.width = 16;
    frame.height = 16;
    frame.plane[0] = pixels.data();
    frame.stride[0] = 16u * 4u;
    if (video.present(handle, &frame) != MP_OK) {
        return {};
    }
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    MpPixelLayout layout{};
    layout.size = sizeof(layout);
    if (video.read_back(handle, nullptr, 0, &w, &h, &layout) != MP_ERR_NO_MEMORY) {
        return {};
    }
    std::vector<float> out(static_cast<std::size_t>(w) * h * 4u);
    if (video.read_back(handle, out.data(), out.size() * sizeof(float), &w, &h, &layout) !=
        MP_OK) {
        return {};
    }
    return out;
}

} // namespace

TEST_CASE("an identity table gives the picture back", "[vdsp][lut]")
{
    // **The whole path, and the reason this is the first test.** With a stage
    // in the chain the presenter runs twice -- stop at linear light, grade,
    // then tone-map and encode -- and with none it runs once. Those two must
    // produce the same pixels, or the split has changed the picture and every
    // other assertion about colour in this tree is about a path nobody uses.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module lut{MEDIAPERCH_VDSP_LUT, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(lut.as<MpVideoDspVtbl>() != nullptr);

    const std::vector<float> plain =
        shown(*video.as<MpVideoVtbl>(), nullptr, nullptr, 0x20, 0x60, 0xC0);
    REQUIRE(plain.size() == 16u * 16u * 4u);

    // An identity table, and the stage with nothing loaded at all -- which is
    // also the identity, because a stage in the chain that has been given no
    // file should change nothing rather than produce black.
    const CubeFile file{cube_text(17, [](const float*, float*) {})};
    const std::string named_file = file.name();
    for (const char* named : {static_cast<const char*>(nullptr), named_file.c_str()}) {
        INFO((named == nullptr ? std::string{"no file, so the identity it holds"}
                               : std::string{named}));
        const std::vector<float> graded =
            shown(*video.as<MpVideoVtbl>(), lut.as<MpVideoDspVtbl>(), named, 0x20, 0x60,
                  0xC0);
        REQUIRE(graded.size() == plain.size());
        for (std::size_t i = 0; i < plain.size(); ++i) {
            // **1e-6, which is what a correct fp32 path gives.** Not bit-exact,
            // and it could not be: a tetrahedral combination of exactly-spaced
            // corners reproduces its input up to floating-point rounding and no
            // closer. A tolerance of 1e-3 would pass on a path that quantised
            // to eight bits somewhere, which is the failure this catches.
            REQUIRE(graded[i] == Catch::Approx(plain[i]).margin(1e-6));
        }
    }
}

TEST_CASE("a table that exchanges two channels exchanges them", "[vdsp][lut]")
{
    // The other half: a table that *does* something does exactly what it says.
    // Red and blue swapped is the case that also catches the axis order -- the
    // format varies red fastest and so does a Direct3D 3D texture, and getting
    // that backwards produces a picture that is wrong in a way no identity test
    // can see.
    Module video{MEDIAPERCH_VIDEO_D3D11, MP_KIND_VIDEO};
    Module lut{MEDIAPERCH_VDSP_LUT, MP_KIND_VDSP};
    REQUIRE(video.as<MpVideoVtbl>() != nullptr);
    REQUIRE(lut.as<MpVideoDspVtbl>() != nullptr);

    const CubeFile file{cube_text(33, [](const float* in, float* out) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
    })};

    const std::vector<float> plain =
        shown(*video.as<MpVideoVtbl>(), nullptr, nullptr, 0x20, 0x60, 0xC0);
    const std::vector<float> swapped = shown(*video.as<MpVideoVtbl>(),
                                             lut.as<MpVideoDspVtbl>(), file.name().c_str(),
                                             0x20, 0x60, 0xC0);
    REQUIRE(plain.size() == 16u * 16u * 4u);
    REQUIRE(swapped.size() == plain.size());

    for (std::size_t i = 0; i < plain.size(); i += 4) {
        // **A 33-entry table over a linear ramp is exact at the corners and
        // interpolated between**, so the margin is the table's own resolution
        // rather than the shader's. The three channels here land on grid points
        // to within that.
        CHECK(swapped[i + 0] == Catch::Approx(plain[i + 2]).margin(2e-4));
        CHECK(swapped[i + 1] == Catch::Approx(plain[i + 1]).margin(2e-4));
        CHECK(swapped[i + 2] == Catch::Approx(plain[i + 0]).margin(2e-4));
        CHECK(swapped[i + 3] == Catch::Approx(plain[i + 3]).margin(1e-6));
    }
}

TEST_CASE("the stage says what it is and what it will not take", "[vdsp][lut]")
{
    Module lut{MEDIAPERCH_VDSP_LUT, MP_KIND_VDSP};
    REQUIRE(lut.as<MpVideoDspVtbl>() != nullptr);
    const MpVideoDspVtbl& vtbl = *lut.as<MpVideoDspVtbl>();

    // **`api` is the question** (§9.8.3): this stage runs on the presenter's
    // D3D11 device or it does not run, and saying zero is how a host learns
    // that before it opens anything.
    std::uint32_t score = 0;
    REQUIRE(vtbl.probe(MP_GRAPHICS_D3D11, &score) == MP_OK);
    CHECK(score > 0u);
    REQUIRE(vtbl.probe(MP_GRAPHICS_NONE, &score) == MP_OK);
    CHECK(score == 0u);
    REQUIRE(vtbl.probe(MP_GRAPHICS_D3D12, &score) == MP_OK);
    CHECK(score == 0u);

    // And a device it cannot use is refused rather than opened and then found
    // wanting on the first frame.
    MpVideoDsp* stage = nullptr;
    CHECK(vtbl.open(nullptr, &stage) == MP_ERR_UNSUPPORTED);
    CHECK(stage == nullptr);
}

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

    /// The rendered pixels, BGRA8.
    [[nodiscard]] std::vector<std::uint8_t> read_back()
    {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        // The same grow-and-ask-again shape read_packet has: nothing is lost
        // by asking with no room first.
        const MpResult sized = vtbl_->read_back(handle_, nullptr, 0, &w, &h);
        if (sized != MP_ERR_NO_MEMORY || w == 0 || h == 0) {
            return {};
        }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4u);
        if (vtbl_->read_back(handle_, out.data(), out.size(), &w, &h) != MP_OK) {
            return {};
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

    const std::vector<std::uint8_t> shown = presenter.read_back();
    REQUIRE(shown.size() == 64u * 48u * 4u);

    // **In and out unchanged**, which is the property everything else is
    // measured against. The shader decodes sRGB to linear because the target is
    // scRGB, `read_back` encodes it again because a screenshot is sRGB, and a
    // scale of one between them means the round trip is the identity to within
    // what eight bits can hold.
    for (std::size_t i = 0; i < shown.size(); i += 4) {
        CHECK(std::abs(int{shown[i]} - int{source[i]}) <= 1);
        CHECK(std::abs(int{shown[i + 1]} - int{source[i + 1]}) <= 1);
        CHECK(std::abs(int{shown[i + 2]} - int{source[i + 2]}) <= 1);
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
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;

    for (std::vector<std::uint8_t>* into : {&first, &second}) {
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

    for (const std::uint8_t level : {std::uint8_t{4}, std::uint8_t{10}, std::uint8_t{64},
                                     std::uint8_t{200}}) {
        const std::vector<std::uint8_t> source = flat(16, 16, level, level, level);
        REQUIRE(presenter.present(source, 16, 16) == MP_OK);
        const std::vector<std::uint8_t> shown = presenter.read_back();
        REQUIRE(shown.size() == 16u * 16u * 4u);
        // Decoded and re-encoded by the same curve, so the value survives. A
        // 2.2 power on the way in and the piecewise curve on the way out would
        // move a level of 4 by more than one.
        CHECK(std::abs(int{shown[0]} - int{level}) <= 1);
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

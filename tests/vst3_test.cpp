// SPDX-License-Identifier: GPL-3.0-or-later
//
// The VST3 host, against a VST3 this repository wrote.
//
// **A host is only tested by running a plugin**, and a test that ran somebody's
// installed plugin would pass or fail depending on what was installed. So
// `tests/vst3_plugin/test_effect.cpp` is a real VST3 -- two objects, an
// `IConnectionPoint` between them, a parameter, a latency and a tail -- built
// beside this file and loaded through the same code path a stranger's plugin
// takes. What is under test is entirely on this side of the boundary.
//
// The two pure decisions are tested first because they are the ones that are
// wrong silently: a bundle path that resolves to nothing produces a clear
// error, but a speaker arrangement that is off by one bit produces audio in the
// wrong channels and no error at all.

#include "vst3_host.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/// Where the build put the test plugin. Passed in by CMake rather than guessed:
/// the path differs between single- and multi-config generators, and a test
/// that searched for it would be testing the search.
const char* plugin_path()
{
    return MEDIAPERCH_TEST_VST3;
}

/// Deinterleaved f64, the shape the bus is in.
struct Bus {
    explicit Bus(std::uint32_t channels, std::uint32_t frames)
        : data(static_cast<std::size_t>(channels) * frames, 0.0), ptr(channels)
    {
        for (std::uint32_t c = 0; c < channels; ++c) {
            ptr[c] = data.data() + static_cast<std::size_t>(c) * frames;
        }
    }
    std::vector<double> data;
    std::vector<double*> ptr;

    [[nodiscard]] const double* const* in() const { return ptr.data(); }
    [[nodiscard]] double* const* out() { return ptr.data(); }
};

} // namespace

// --------------------------------------------------------------------------
// The decisions that are ordinary logic
// --------------------------------------------------------------------------

TEST_CASE("a .vst3 that is a directory is a bundle with the DLL inside it",
          "[vst3][bundle]")
{
    // The plugin the tests build is a plain DLL, so it comes back unchanged --
    // which is the whole of the old-style case.
    const std::wstring plain = mp::vst3::binary_in_bundle(
        std::wstring{plugin_path(), plugin_path() + std::strlen(plugin_path())});
    CHECK(plain.find(L"Contents") == std::wstring::npos);

    // A path that does not exist is left alone too: the caller reports "cannot
    // load X" with the name a person typed, which is more use than a message
    // about a directory nobody mentioned.
    CHECK(mp::vst3::binary_in_bundle(L"C:\\nowhere\\Missing.vst3") ==
          L"C:\\nowhere\\Missing.vst3");
    CHECK(mp::vst3::binary_in_bundle(L"").empty());

    // And a real directory becomes the bundle path. `C:\Windows` is not a
    // plugin, but it is reliably a directory, which is the property under test.
    const std::wstring bundle = mp::vst3::binary_in_bundle(L"C:\\Windows");
    CHECK(bundle == L"C:\\Windows\\Contents\\x86_64-win\\Windows");
    // A trailing separator is not part of the name.
    CHECK(mp::vst3::binary_in_bundle(L"C:\\Windows\\") == bundle);
}

TEST_CASE("channel layouts become the arrangement VST3 names", "[vst3][speakers]")
{
    using namespace Steinberg::Vst;

    // **A mask is already an arrangement.** The two vocabularies are the same
    // eighteen bits in the same order, which is worth an assertion rather than
    // a comment: the first version of `arrangement_for` had a table that
    // translated 0x63F into k71Music, and k71Music is 0x63F.
    CHECK(SpeakerArr::kStereo == 0x3);
    CHECK(SpeakerArr::k51 == 0x3F);
    CHECK(SpeakerArr::k71Music == 0x63F); // Windows 7.1, bit for bit
    CHECK(mp::vst3::arrangement_for(2, 0x3) == 0x3);
    CHECK(mp::vst3::arrangement_for(6, 0x3F) == SpeakerArr::k51);
    CHECK(mp::vst3::arrangement_for(8, 0x63F) == SpeakerArr::k71Music);
    // Including the cinema layout, which is a different eight speakers and
    // survives precisely because nothing translates it.
    CHECK(mp::vst3::arrangement_for(8, 0xFF) == SpeakerArr::k71Cine);

    // Without a mask -- the non-extensible form, and most of what arrives here
    // -- the conventional layout for the count.
    CHECK(mp::vst3::arrangement_for(1, 0) == SpeakerArr::kMono);
    CHECK(mp::vst3::arrangement_for(2, 0) == SpeakerArr::kStereo);
    CHECK(mp::vst3::arrangement_for(6, 0) == SpeakerArr::k51);
    CHECK(mp::vst3::arrangement_for(8, 0) == SpeakerArr::k71Music);

    // A mask whose population disagrees with the channel count is not trusted:
    // the buffers are sized by the count, so the count wins.
    CHECK(mp::vst3::channels_in(mp::vst3::arrangement_for(2, 0x3F)) == 2);

    // And whatever else arrives, the count is right even when the layout is a
    // guess, because the count is what a plugin refuses on.
    for (std::uint32_t c = 1; c <= 16; ++c) {
        CHECK(mp::vst3::channels_in(mp::vst3::arrangement_for(c, 0)) == c);
    }
}

// --------------------------------------------------------------------------
// The part that talks to a plugin
// --------------------------------------------------------------------------

TEST_CASE("a plugin that is not one is refused with a reason", "[vst3][load]")
{
    mp::vst3::Host host;
    std::string why;

    SECTION("nothing there")
    {
        CHECK_FALSE(host.load("C:\\nowhere\\Missing.vst3", "", why));
        CHECK(why.find("Missing.vst3") != std::string::npos);
    }

    SECTION("a DLL that is not a VST3")
    {
        // A real Windows DLL with no GetPluginFactory. The message has to say
        // that rather than "cannot load", because the two are fixed differently.
        CHECK_FALSE(host.load("C:\\Windows\\System32\\kernel32.dll", "", why));
        CHECK(why.find("GetPluginFactory") != std::string::npos);
    }

    SECTION("no class by that name")
    {
        CHECK_FALSE(host.load(plugin_path(), "Reverb", why));
        // And it says what there was instead, which is the only way to find out
        // without an editor.
        CHECK(why.find("Test Effect") != std::string::npos);
    }
}

TEST_CASE("a VST3 in the chain gets the bus and gives it back", "[vst3][process]")
{
    mp::vst3::Host host;
    std::string why;
    REQUIRE(host.load(plugin_path(), "", why));
    CHECK(host.name() == "MediaPerch Test Effect");
    CHECK(host.vendor() == "MediaPerch");
    CHECK(host.subcategories() == "Fx|Tools");

    constexpr std::uint32_t channels = 2;
    constexpr std::uint32_t frames = 512;
    REQUIRE(host.configure(channels, 0x3, 48000.0, frames, why));
    CHECK(host.active());
    // Both declared by the plugin, and both only knowable once it is set up.
    CHECK(host.latency_frames() == 64);
    CHECK(host.tail_frames() == 128);

    Bus in{channels, frames};
    Bus out{channels, frames};
    for (std::uint32_t c = 0; c < channels; ++c) {
        for (std::uint32_t n = 0; n < frames; ++n) {
            in.ptr[c][n] = 0.5 * std::sin(0.01 * n) + 0.1 * c;
        }
    }

    SECTION("at unity it is the audio it was given")
    {
        REQUIRE(host.set_parameter("Gain", 1.0, why));
        REQUIRE(host.process(in.in(), frames, out.out()));
        for (std::uint32_t c = 0; c < channels; ++c) {
            for (std::uint32_t n = 0; n < frames; ++n) {
                // Approximate rather than exact: this plugin declares f32 only,
                // so the samples were narrowed and widened around it. That is
                // the cost the host reports as `precision: f32`.
                CHECK(out.ptr[c][n] == Catch::Approx(in.ptr[c][n]).margin(1e-6));
            }
        }
    }

    SECTION("a parameter reaches the samples")
    {
        REQUIRE(host.set_parameter("Gain", 0.25, why));
        REQUIRE(host.process(in.in(), frames, out.out()));
        for (std::uint32_t c = 0; c < channels; ++c) {
            for (std::uint32_t n = 0; n < frames; ++n) {
                CHECK(out.ptr[c][n] == Catch::Approx(in.ptr[c][n] * 0.25).margin(1e-6));
            }
        }
    }

    SECTION("by id as well as by name")
    {
        REQUIRE(host.set_parameter("42", 0.5, why));
        REQUIRE(host.process(in.in(), frames, out.out()));
        CHECK(out.ptr[0][10] == Catch::Approx(in.ptr[0][10] * 0.5).margin(1e-6));
    }

    SECTION("a parameter that is not there says what is")
    {
        CHECK_FALSE(host.set_parameter("Wetness", 0.5, why));
        CHECK(why.find("Gain") != std::string::npos);
        // Out of range is a different complaint, and worth a different message:
        // VST3 parameters are normalised and a person typing decibels is making
        // a specific mistake.
        CHECK_FALSE(host.set_parameter("Gain", 3.0, why));
        CHECK(why.find("normalised") != std::string::npos);
    }

    SECTION("its state arrives as bytes")
    {
        const double half = 0.5;
        std::vector<std::uint8_t> bytes(sizeof(half));
        std::memcpy(bytes.data(), &half, sizeof(half));
        REQUIRE(host.set_state(bytes, why));
        REQUIRE(host.process(in.in(), frames, out.out()));
        CHECK(out.ptr[1][20] == Catch::Approx(in.ptr[1][20] * 0.5).margin(1e-6));
    }

    SECTION("and the parameter it has is the one it says it has")
    {
        REQUIRE(host.parameter_count() == 1);
        std::string title;
        std::string shown;
        double value = 0.0;
        REQUIRE(host.parameter(0, title, value, shown));
        CHECK(title == "Gain");
        CHECK(value == Catch::Approx(1.0));
        CHECK(shown == "1.00");
    }
}

TEST_CASE("a VST3 is reconfigured rather than reloaded", "[vst3][configure]")
{
    mp::vst3::Host host;
    std::string why;
    REQUIRE(host.load(plugin_path(), "0", why)); // by index, the other spelling

    // What the graph does: once to find out the shape, once with the block the
    // device turned out to want.
    REQUIRE(host.configure(2, 0x3, 44100.0, 4096, why));
    REQUIRE(host.configure(2, 0x3, 44100.0, 480, why));
    CHECK(host.active());

    Bus in{2, 480};
    Bus out{2, 480};
    in.ptr[0][0] = 1.0;
    REQUIRE(host.process(in.in(), 480, out.out()));

    // A block larger than the one it agreed to is refused rather than passed on:
    // the plugin was told a maximum and writing past it is what it would do.
    Bus big_out{2, 4096};
    Bus big_in{2, 4096};
    CHECK_FALSE(host.process(big_in.in(), 4096, big_out.out()));

    // A seek. VST3 has no reset, so this is a deactivate and reactivate, and
    // what has to be true is that it can still process afterwards.
    CHECK(host.reset());
    REQUIRE(host.process(in.in(), 480, out.out()));
}

TEST_CASE("a plugin that takes doubles is handed the bus untouched", "[vst3][precision]")
{
    // The test plugin declares f32 only, which is the common case and the one
    // that costs a conversion each way. `native_f64` is what `describe` reports,
    // and it is the difference between a stage that could be exact and one that
    // cannot.
    mp::vst3::Host host;
    std::string why;
    REQUIRE(host.load(plugin_path(), "", why));
    REQUIRE(host.configure(2, 0x3, 48000.0, 256, why));
    CHECK_FALSE(host.native_f64());

    // The proof that it is a conversion and not a copy: a value that has no f32
    // representation does not survive.
    Bus in{2, 256};
    Bus out{2, 256};
    const double awkward = 1.0 / 3.0;
    in.ptr[0][0] = awkward;
    REQUIRE(host.set_parameter("Gain", 1.0, why));
    REQUIRE(host.process(in.in(), 256, out.out()));
    CHECK(out.ptr[0][0] != awkward);
    CHECK(out.ptr[0][0] == Catch::Approx(awkward).margin(1e-7));
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Cube LUT reader, which is a parser and is therefore where the bugs are.
//
// **No GPU in this file, on purpose.** §9.8.3's lookup-table stage needs a
// device; reading the text does not, and a parser that needed one would be a
// parser nobody could fuzz. What is checked here is the reading: the format's
// own rules, the refusals, and the one assertion a lookup table can be held to
// with no reference at all -- that an identity table is the identity.

#include "cube.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

/// A cube written out the way a grading tool writes one.
std::string written(std::uint32_t size, const char* extra = "")
{
    std::string out = "TITLE \"a test\"\n";
    out += extra;
    out += "LUT_3D_SIZE " + std::to_string(size) + "\n";
    const auto step = 1.0f / static_cast<float>(size - 1);
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                out += std::to_string(static_cast<float>(r) * step) + " " +
                       std::to_string(static_cast<float>(g) * step) + " " +
                       std::to_string(static_cast<float>(b) * step) + "\n";
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("a cube LUT is read the way its format states it", "[cube]")
{
    const mp::CubeText read = mp::parse_cube(written(4));
    REQUIRE(read.ok);
    CHECK(read.why.empty());
    CHECK(read.lut.title == "a test");
    CHECK(read.lut.size == 4u);
    CHECK(read.lut.table.size() == 4u * 4u * 4u * 3u);

    // **Red varies fastest**, which is the format's order and is also Direct3D's
    // for a 3D texture -- so what comes out of here uploads without being
    // rearranged, and getting it backwards would be a picture with its red and
    // blue exchanged in a way no other test would catch.
    CHECK(read.lut.table[0] == Catch::Approx(0.0f));
    CHECK(read.lut.table[3] == Catch::Approx(1.0f / 3.0f).margin(1e-5));
    CHECK(read.lut.table[4] == Catch::Approx(0.0f));
    // The second *plane* is the first blue step.
    const std::size_t plane = 4u * 4u * 3u;
    CHECK(read.lut.table[plane + 2] == Catch::Approx(1.0f / 3.0f).margin(1e-5));
}

TEST_CASE("an identity table is the identity, which is the one free assertion",
          "[cube]")
{
    // Nothing else about a lookup table can be checked without a reference: a
    // grade is somebody's taste. This can, and it is what the stage's own test
    // rests on -- a picture through an identity LUT must come back unchanged.
    for (const std::uint32_t size : {2u, 4u, 17u, 33u}) {
        INFO("size " << size);
        const mp::CubeLut made = mp::identity_cube(size);
        CHECK(made.size == size);
        CHECK(made.identity());
        // And it survives being written as six decimal places and read back.
        const mp::CubeText read = mp::parse_cube(written(size));
        REQUIRE(read.ok);
        CHECK(read.lut.identity(1e-5f));
    }

    // A table that is not the identity says so, and the check is exact by
    // default: `to_string`'s six places are not free, which is why the file
    // above is read with a tolerance and the computed one without.
    mp::CubeLut moved = mp::identity_cube(4);
    moved.table[3 * 5 + 1] += 0.01f;
    CHECK_FALSE(moved.identity());
    CHECK_FALSE(moved.identity(1e-5f));
}

TEST_CASE("the domain is read, because a log LUT states one", "[cube]")
{
    const mp::CubeText read =
        mp::parse_cube(written(2, "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 4.0 4.0 4.0\n"));
    REQUIRE(read.ok);
    CHECK(read.lut.domain_max[0] == Catch::Approx(4.0f));
    // **And it is not the identity any more**, whatever the table says: a cube
    // over 0..4 that maps 0..1 onto itself is a different function from one
    // over 0..1, and calling both identity would be calling two things one.
    CHECK_FALSE(read.lut.identity(1e-5f));
}

TEST_CASE("a file that is not a cube is refused with the line", "[cube]")
{
    struct Case {
        const char* text;
        const char* mentions;
    };
    const Case cases[] = {
        {"TITLE \"nothing else\"\n", "no LUT_3D_SIZE"},
        {"LUT_1D_SIZE 16\n0 0 0\n", "1D LUT"},
        {"LUT_3D_SIZE 1\n", "2 to 256"},
        {"LUT_3D_SIZE 300\n", "2 to 256"},
        {"LUT_3D_SIZE 4000000000\n", "2 to 256"},
        {"LUT_3D_SIZE two\n", "2 to 256"},
        {"0.0 0.0 0.0\nLUT_3D_SIZE 2\n", "before LUT_3D_SIZE"},
        {"LUT_3D_SIZE 2\n0 0 0\n", "rows for a size 2 cube"},
        {"LUT_3D_SIZE 2\n0 0\n", "three finite numbers"},
        {"LUT_3D_SIZE 2\n0 0 nan\n", "three finite numbers"},
        {"LUT_3D_SIZE 2\n0 0 inf\n", "three finite numbers"},
        {"LUT_3D_SIZE 2\n0 0 0 0\n", "three finite numbers"},
        {"LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", "stated twice"},
        {"LUT_3D_SIZE 2\nDOMAIN_MIN 0 0\n", "DOMAIN_MIN is three numbers"},
    };
    for (const Case& one : cases) {
        INFO(one.text);
        const mp::CubeText read = mp::parse_cube(one.text, "x.cube");
        CHECK_FALSE(read.ok);
        CHECK(read.why.find(one.mentions) != std::string::npos);
    }

    // **A size that would allocate before it is checked** is the one that
    // matters: `LUT_3D_SIZE 4000000000` in a two-line file is sixty-four
    // gigabytes if the bound comes after the resize instead of before it.
    // Above, and it comes back as a sentence.

    // An empty domain is refused rather than divided by.
    const mp::CubeText flat =
        mp::parse_cube(written(2, "DOMAIN_MIN 1 1 1\nDOMAIN_MAX 1 1 1\n"));
    CHECK_FALSE(flat.ok);
    CHECK(flat.why.find("domain is empty") != std::string::npos);
}

TEST_CASE("comments, blank lines and carriage returns are what files have in them",
          "[cube]")
{
    // A file written on Windows and read anywhere else is the ordinary case,
    // and a stray carriage return would land inside the last number on every
    // line -- which reads as a table of nothing but the first two channels.
    std::string text = "# made by something\r\n\r\nTITLE \"crlf\"\r\nLUT_3D_SIZE 2\r\n";
    for (int i = 0; i < 8; ++i) {
        text += "0.5 0.25 0.125\r\n";
    }
    const mp::CubeText read = mp::parse_cube(text);
    REQUIRE(read.ok);
    CHECK(read.lut.title == "crlf");
    CHECK(read.lut.table[2] == Catch::Approx(0.125f));

    // A file with nothing in it is not a cube, and says so rather than
    // producing an empty one that would grade everything to black.
    CHECK_FALSE(mp::parse_cube("").ok);
}

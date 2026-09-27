// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every demuxer, handed broken copies of real files.
//
// **What a fuzzer finds with time, this finds every run, for the breakage that
// is certain to happen**: a file cut short by a download that stopped, a bit
// gone wrong, a length field that says four billion or nothing at all. Every
// demuxer this tree builds is made to read every such copy of every seed file
// the fuzzers start from -- whichever container the file is, since forcing a
// demuxer on a file that is not its own is how the format matrix found
// demux_mkv reading a dummy element as a header. It runs in every CI leg, the
// Debug ones among them, where the STL's iterator checks are on and a fuzzer
// is not.
//
// Each read is the host's (tests/demux_drive.hpp), with module.h's promises
// held to the answers; what counts as failing is a broken promise, or a crash.

#include "demux_drive.hpp"
#include "module_loader.hpp"
#include "scratch_files.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

/// Every demuxer the build put in the module directory, found rather than
/// named: the one nobody remembered to name is the one that falls behind.
std::vector<std::filesystem::path> demuxers()
{
    std::vector<std::filesystem::path> found;
    std::error_code trouble;
    const std::filesystem::path dir = std::filesystem::path{MEDIAPERCH_MODULE_DIR} / "demux";
    for (const auto& entry : std::filesystem::directory_iterator{dir, trouble}) {
        if (entry.path().extension() == ".dll") {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

/// The fuzzers' seed files, one directory a container, and the Matroska and
/// WebM files the tests read.
std::vector<std::filesystem::path> seeds()
{
    const std::filesystem::path root{MEDIAPERCH_SOURCE_DIR};
    const char* const dirs[] = {"fuzz/corpus/wav",     "fuzz/corpus/flac", "fuzz/corpus/mp3",
                                "fuzz/corpus/adts",    "fuzz/corpus/dsd",  "fuzz/corpus/wavpack",
                                "fuzz/corpus/mp4",     "fuzz/corpus/ogg",  "tests/data/mkv"};
    std::vector<std::filesystem::path> found;
    std::error_code trouble;
    for (const char* dir : dirs) {
        for (const auto& entry : std::filesystem::directory_iterator{root / dir, trouble}) {
            if (entry.is_regular_file(trouble)) {
                found.push_back(entry.path());
            }
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

struct Broken {
    std::string file;
    std::string demuxer;
    std::vector<std::string>* failures;
    void operator()(const char* promise) const
    {
        failures->push_back(demuxer + " on " + file + ": " + promise);
    }
};

/// A copy with one thing wrong, and what to call it in a failure.
struct Variant {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

/// The ways a real file goes wrong: cut short anywhere, a bit flipped, and a
/// length made huge or made nothing -- four bytes of 0xFF or of 0x00, which in
/// every container here is a size field somewhere near the front.
std::vector<Variant> broken_copies(const std::vector<std::uint8_t>& file)
{
    std::vector<Variant> out;
    const std::size_t n = file.size();
    const std::size_t cuts[] = {0, 1, 4, 8, 16, 32, 64, n / 4, n / 2, 3 * n / 4, n - 1};
    std::vector<std::size_t> made;
    for (std::size_t cut : cuts) {
        if (cut < n && std::find(made.begin(), made.end(), cut) == made.end()) {
            made.push_back(cut);
            out.push_back({"cut to " + std::to_string(cut) + " bytes",
                           std::vector<std::uint8_t>(file.begin(),
                                                     file.begin() + static_cast<std::ptrdiff_t>(cut))});
        }
    }
    for (std::size_t k = 0; k < 16 && n != 0; ++k) {
        const std::size_t at = (k * n) / 16 + (k * 7) % 16 < n ? (k * n) / 16 + (k * 7) % 16 : n - 1;
        Variant v{"bit " + std::to_string(k % 8) + " of byte " + std::to_string(at) + " flipped",
                  file};
        v.bytes[at] = static_cast<std::uint8_t>(v.bytes[at] ^ (1u << (k % 8)));
        out.push_back(std::move(v));
    }
    const std::size_t places[] = {4, 8, 12, 16, 24, n / 3, 2 * n / 3};
    for (std::size_t at : places) {
        for (const std::uint8_t fill : {std::uint8_t{0xFF}, std::uint8_t{0x00}}) {
            if (at + 4 > n) {
                continue;
            }
            Variant v{"bytes " + std::to_string(at) + " to " + std::to_string(at + 3) +
                          (fill != 0 ? " made 0xFF" : " made 0x00"),
                      file};
            std::fill_n(v.bytes.begin() + static_cast<std::ptrdiff_t>(at), 4, fill);
            out.push_back(std::move(v));
        }
    }
    return out;
}

} // namespace

TEST_CASE("every demuxer reads broken copies of real files and lives", "[demux][robust]")
{
    const auto found = demuxers();
    REQUIRE(found.size() >= 8u);
    const auto files = seeds();
    REQUIRE(files.size() >= 20u);

    // A deque, because a Module is neither copied nor moved: it owns a loaded
    // library, and a vector growing would have to move one.
    std::deque<mp::test::Module> modules;
    std::vector<std::string> names;
    for (const auto& path : found) {
        modules.emplace_back(path.string().c_str(), MP_KIND_DEMUX);
        names.push_back(path.stem().string());
    }

    // Smaller bounds than a fuzzer's: this reads every copy with every
    // demuxer, and a broken promise shows in the first packets or not at all.
    mp::test::DriveLimits limits;
    limits.packets = 256;
    limits.packets_after_seek = 16;

    mp::test::ScratchFiles scratch{"mediaperch-demux-robust-"};
    std::vector<std::string> failures;
    std::size_t reads = 0;
    for (const auto& file : files) {
        std::ifstream in{file, std::ios::binary};
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in},
                                              std::istreambuf_iterator<char>{}};
        const auto copies = broken_copies(bytes);
        for (std::size_t c = 0; c < copies.size(); ++c) {
            const Variant& copy = copies[c];
            const char* path = scratch.hold(copy.bytes.data(), copy.bytes.size());
            REQUIRE(path != nullptr);
            for (std::size_t m = 0; m < modules.size(); ++m) {
                const MpDemuxVtbl* v = modules[m].as<MpDemuxVtbl>();
                if (v == nullptr) {
                    continue; // module_abi_test says which would not load
                }
                Broken broken{file.filename().string() + ", " + copy.name, names[m], &failures};
                mp::test::DemuxDrive<Broken> drive{*v, limits, broken};
                // The seeks: somewhere in the file, a stream, chosen by which
                // copy this is so that every run makes the same ones.
                drive.run(path, copy.bytes.data(), copy.bytes.size(),
                          0x9E3779B97F4A7C15ull * (c + 1));
                ++reads;
            }
        }
    }
    // Every demuxer found was asked: one that would not load is a gap here as
    // well as a failure there, and this is not the test to let it pass.
    std::size_t loaded = 0;
    for (const auto& module : modules) {
        loaded += module.as<MpDemuxVtbl>() != nullptr ? 1u : 0u;
    }
    CHECK(loaded == modules.size());
    INFO(reads << " reads of " << files.size() << " files' broken copies by " << loaded
               << " demuxers");
    std::string listed;
    for (const std::string& failure : failures) {
        listed += "\n  " + failure;
    }
    INFO("promises broken:" << listed);
    CHECK(failures.empty());
}

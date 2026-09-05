// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every module this tree builds answers at the ABI this tree is on.
//
// **Five modules were dead for a day and nothing said so.** ABI v4 changed the
// video half -- MpPixelFormat became MpPixelLayout, MpVideoFrame grew -- and
// `modules/shared/mp-abi`, which is the Rust mirror of the header, carries no
// video structure at all. So there was nothing in it to update, no compile
// error, and its `ABI_VERSION` stayed at 3 while the header went to 4.
// `mp_module_entry` answers null to a host on a different version, which is
// exactly what a version bump is for -- so codec_aac, codec_alac, codec_dsd,
// demux_adts and demux_dsd stopped loading. AAC and ALAC became FFmpeg's
// quietly, and DSD lost its bit-exact DoP path and came out as F32.
//
// Nothing caught it. The C++ tests load the modules they name and none of them
// names those five; `rust_modules` builds the crates and runs their own tests,
// which know nothing about a host; and `format_matrix`, which would have shown
// it in one line, needs FFmpeg and a corpus and so skips in every CI leg that
// builds -- the one job that has FFmpeg runs `decode_quality` alone.
//
// So this walks the module directory instead of naming anything. A module is a
// DLL exporting `mp_module_entry`; what it must do is answer the version this
// tree is compiled for. Anything that does not is a module the player will not
// load, and a silent absence is the one thing §7 says this tree does not do.

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

/// Every DLL under the directory the modules are built into, whatever built
/// them. Naming them would defeat the point: the module that falls behind is
/// the one nobody remembered to name.
std::vector<std::filesystem::path> modules()
{
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root{MEDIAPERCH_MODULE_DIR};
    std::error_code trouble;
    if (!std::filesystem::is_directory(root, trouble)) {
        return found;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, trouble)) {
        if (entry.is_regular_file(trouble) && entry.path().extension() == ".dll") {
            found.push_back(entry.path());
        }
    }
    return found;
}

} // namespace

TEST_CASE("every module built here loads at this tree's ABI version", "[abi][modules]")
{
    const auto found = modules();
    // A build that produced no modules would pass every assertion below by
    // having none to make, which is the failure mode a sweep like this has.
    REQUIRE(found.size() >= 10u);

    for (const auto& path : found) {
        const std::string name = path.filename().string();
        INFO(name);

        auto* dll = ::LoadLibraryW(path.c_str());
        REQUIRE(dll != nullptr);

        using Entry = const MpModuleDesc*(MP_CALL*)(std::uint32_t);
        auto* entry = reinterpret_cast<Entry>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "mp_module_entry")));
        // Every DLL in this directory is a module, so one without the export is
        // something that should not have been put here.
        REQUIRE(entry != nullptr);

        const MpModuleDesc* desc = entry(MP_ABI_VERSION);
        // **The line the Rust mirror would have failed.** A module that answers
        // null here is one the player silently does without.
        REQUIRE(desc != nullptr);
        CHECK(desc->abi_version == MP_ABI_VERSION);
        CHECK(desc->size == sizeof(MpModuleDesc));
        CHECK(desc->id != nullptr);
        CHECK(desc->vtbl != nullptr);
        // §4 rule 6: what a module can do is data, readable without running it.
        // A module that reports codecs must list them, and one that lists none
        // must say zero rather than pointing somewhere.
        CHECK((desc->codec_count == 0u) == (desc->codecs == nullptr));

        // And a version this tree is not on gets nothing, which is the other
        // half of the same promise.
        CHECK(entry(MP_ABI_VERSION + 1u) == nullptr);
        CHECK(entry(MP_ABI_VERSION - 1u) == nullptr);

        if (desc->shutdown != nullptr) {
            desc->shutdown();
        }
        ::FreeLibrary(dll);
    }
}

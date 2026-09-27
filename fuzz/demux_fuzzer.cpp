// SPDX-License-Identifier: GPL-3.0-or-later
//
// A demuxer module, through the module ABI, given arbitrary bytes as a file.
//
// **The module, and not only the parser inside it.** The other fuzzers here
// hand a parser its bytes -- dr_wav, libmpg123, Bento4 -- and what sits
// between such a parser and the host was fuzzed by nothing: the module's own
// reading of the container, its index, its packets, its seek. That is where
// the Matroska demuxer took whatever libebml found for the header it had asked
// for. The format matrix found it by making every demuxer read every file;
// this makes one demuxer read every file libFuzzer can make.
//
// Built once per demuxer, with the module's source and every library under it
// compiled in and instrumented (fuzz/CMakeLists.txt), and driven the way the
// host drives one, with what module.h promises about the answers held to them
// (tests/demux_drive.hpp). A broken promise is a crash here, the same as a bad
// read. The input reaches the module as a file (tests/scratch_files.hpp).

#include "demux_drive.hpp"
#include "module_harness.hpp"
#include "scratch_files.hpp"

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>

namespace {

using mp::fuzz::broken;

struct Abort {
    void operator()(const char* promise) const { broken(promise); }
};

const MpDemuxVtbl& demuxer()
{
    static const MpDemuxVtbl* const vtbl = [] {
        const MpModuleDesc* desc = mp_module_entry(MP_ABI_VERSION);
        if (desc == nullptr || desc->kind != MP_KIND_DEMUX || desc->vtbl == nullptr) {
            broken("mp_module_entry names a demuxer");
        }
        if (desc->init != nullptr && desc->init(&mp::fuzz::host) != MP_OK) {
            broken("init succeeds with a working host");
        }
        return static_cast<const MpDemuxVtbl*>(desc->vtbl);
    }();
    return *vtbl;
}

mp::test::ScratchFiles& files()
{
    static mp::test::ScratchFiles instance{"mediaperch-demux-fuzz-"};
    return instance;
}

/// The last eight bytes of the input, as a number to seek to: the fuzzer owns
/// them as it owns the rest, so the seeks it tries are chosen, not fixed.
std::uint64_t tail_number(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t value = 0;
    const std::size_t from = size > 8 ? size - 8 : 0;
    for (std::size_t i = from; i < size; ++i) {
        value = value << 8 | data[i];
    }
    return value;
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    (void)demuxer();
    (void)files();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const char* const path = files().hold(data, size);
    if (path == nullptr) {
        return 0;
    }
    // libFLAC's allocations, from the last byte: the end of a FLAC file is the
    // last frame's audio, which moves nothing else.
    mp::fuzz::fail_flac_allocations(size != 0 ? data[size - 1] : 0);
    static const mp::test::DriveLimits limits;
    Abort abort;
    mp::test::DemuxDrive<Abort> drive{demuxer(), limits, abort};
    drive.run(path, data, size, tail_number(data, size));
    return 0;
}

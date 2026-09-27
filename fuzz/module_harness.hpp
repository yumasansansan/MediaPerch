// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the fuzzers that drive a module through the module ABI share: the host
// they hand it, the way a broken promise ends the run, and -- in the ones that
// link libFLAC's fuzzing build -- the counters that make libFLAC fail an
// allocation.

#pragma once

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi);

#ifdef MEDIAPERCH_FUZZ_FLAC_ALLOCATIONS
// **libFLAC's fuzzing build fails the allocation it is told to**, and leaves
// the telling to the fuzzer: these three are its (include/share/alloc.h). Each
// input sets them afresh, through fail_flac_allocations(), so that a module
// which takes a failed decoder for a working one is found as surely as a bad
// read. Inline so that a header defines them once per binary; C so that
// libFLAC's references find them.
extern "C" {
inline int alloc_check_threshold = INT32_MAX;
inline int alloc_check_counter = 0;
inline int alloc_check_keep_failing = 0;
}
#endif

namespace mp::fuzz {

/// Ends the run for a module that broke a promise module.h makes, naming the
/// promise; libFuzzer keeps the input, as it does for a bad read.
[[noreturn]] inline void broken(const char* promise)
{
    std::fprintf(stderr, "the module broke module.h's promise: %s\n", promise);
    std::fflush(stderr);
    std::abort();
}

inline void MP_CALL quiet(void*, MpLogLevel, const char*) {}
inline void* MP_CALL take(void*, std::size_t bytes)
{
    return std::malloc(bytes);
}
inline void MP_CALL give(void*, void* p)
{
    std::free(p);
}

/// A host that logs nowhere and allocates with malloc.
inline constexpr MpHost host = {sizeof(MpHost), 0, nullptr, &quiet, &take, &give};

/// Which of libFLAC's allocations fail in the input about to run, from one byte
/// the fuzzer owns: top bit set, the one its low six bits count to fails, and
/// every one after it too when the next bit is also set; top bit clear, none.
/// Nothing, in a fuzzer without libFLAC's fuzzing build.
inline void fail_flac_allocations([[maybe_unused]] std::uint8_t control) noexcept
{
#ifdef MEDIAPERCH_FUZZ_FLAC_ALLOCATIONS
    alloc_check_counter = 0;
    alloc_check_threshold = (control & 0x80u) != 0 ? (control & 0x3Fu) : INT32_MAX;
    alloc_check_keep_failing = (control & 0x40u) != 0 ? 1 : 0;
#endif
}

} // namespace mp::fuzz

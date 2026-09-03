// SPDX-License-Identifier: GPL-3.0-or-later
//
// The section-boundary symbols SanitizerCoverage expects on Windows.
//
// On ELF the linker synthesises `__start_<section>` and `__stop_<section>` for
// any section whose name is a valid identifier, and LLVM's coverage pass leans
// on that: the 8-bit counters go in one section and the module constructor
// hands libFuzzer the range between those two symbols. COFF has no such
// feature. What it has is `$`-suffixed section grouping -- the linker merges
// `.SCOV$CA`, `.SCOV$CM` and `.SCOV$CZ` into one `.SCOV`, sorted by suffix --
// so the convention is to define the start symbol in `$CA` and the stop symbol
// in `$CZ`, with the instrumented objects' `$CM` landing between them. That is
// what compiler-rt's sanitizer runtime does, and clang links that runtime for
// `-fsanitize=fuzzer`. rustc does not, and libfuzzer-sys ships libFuzzer alone.
//
// So this file is that runtime's one relevant page, written for the Rust fuzz
// crates: six variables in the right sections and nothing else. Each sentinel
// is one zero counter libFuzzer sees and never sees change, which it tolerates.
// Every Rust fuzz crate's build.rs compiles this same file, which is why it is
// under modules/shared rather than beside any one of them.
//
// Measured before it existed: `error LNK2001: __start___sancov_cntrs is
// unresolved`, from every instrumented object.

#include <stdint.h>

#pragma section(".SCOV$CA", read, write)
#pragma section(".SCOV$CZ", read, write)
#pragma section(".SCOV$BA", read, write)
#pragma section(".SCOV$BZ", read, write)
#pragma section(".SCOVP$A", read, write)
#pragma section(".SCOVP$Z", read, write)

__declspec(allocate(".SCOV$CA")) uint64_t __start___sancov_cntrs = 0;
__declspec(allocate(".SCOV$CZ")) uint64_t __stop___sancov_cntrs = 0;
__declspec(allocate(".SCOV$BA")) uint64_t __start___sancov_bools = 0;
__declspec(allocate(".SCOV$BZ")) uint64_t __stop___sancov_bools = 0;
__declspec(allocate(".SCOVP$A")) uint64_t __start___sancov_pcs = 0;
__declspec(allocate(".SCOVP$Z")) uint64_t __stop___sancov_pcs = 0;

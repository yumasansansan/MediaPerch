// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module, loaded the way the engine loads one -- once, for every test.
//
// **Seven copies, and five of them had drifted.** Each test that drives a real
// module had its own `struct Module`: the same LoadLibrary, the same
// GetProcAddress, the same version check, in seven files -- and when
// `module_shutdown` turned out to matter (codec_mft deadlocked in a static
// destructor because nothing called it), the fix had to be found and applied
// four times. Two of the seven took a kind, one took nothing, one returned a
// typed vtable and the rest a void pointer. This is the one they all meant.
//
// Deliberately not `mp::win::ModuleRegistry`: what the tests exercise is the
// portable half against a module, and the registry belongs to the Windows head.
// What a module *is*, is a shared library -- a DLL on Windows, a shared object
// elsewhere -- exporting `mp_module_entry`, and saying so in a dozen lines is a
// better statement of the ABI than borrowing the loader that already knows.
// The loading itself is test_platform.hpp's, where the build chooses the
// system. `module_abi_test.cpp` loads modules without this on purpose: it
// needs the entry point itself, to ask it the wrong versions.

#pragma once

#include "test_platform.hpp"

#include <mediaperch/module.h>

#include <cstdint>

namespace mp::test {

struct Module {
    /// The module at `path`, which is UTF-8, as every path the tests are
    /// handed by CMake is.
    Module(const char* path, MpKind kind)
    {
        library = platform::open_library(utf8_path(path));
        if (library == nullptr) {
            return;
        }
        const MpModuleEntry entry = platform::find_module_entry(library);
        if (entry == nullptr) {
            return;
        }
        // **The version check is the break, made visible.** A module built
        // against an older ABI answers null here, which is what a bump is for.
        const MpModuleDesc* found = entry(MP_ABI_VERSION);
        if (found == nullptr || found->kind != kind) {
            return;
        }
        desc = found;
        vtbl = found->vtbl;
    }
    ~Module()
    {
        if (library != nullptr) {
            // Before the library is let go, and not from a destructor inside the
            // module: codec_mft stops Media Foundation here, and MFShutdown from
            // under the loader lock is a deadlock.
            if (desc != nullptr && desc->shutdown != nullptr) {
                desc->shutdown();
            }
            platform::close_library(library);
        }
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    /// The vtable, as the kind it was asked for. Null when the module did not
    /// load, so a test's first REQUIRE is `as<...>() != nullptr`.
    template <class Vtbl>
    [[nodiscard]] const Vtbl* as() const noexcept
    {
        return static_cast<const Vtbl*>(vtbl);
    }

    void* library = nullptr;
    const MpModuleDesc* desc = nullptr;
    const void* vtbl = nullptr;
};

} // namespace mp::test

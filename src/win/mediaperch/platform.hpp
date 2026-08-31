// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Windows head: everything the portable core is handed rather than knows.

#include "mediaperch/source.hpp"

#include <mediaperch/module.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace mp::win {

// --------------------------------------------------------------------------
// Logging
// --------------------------------------------------------------------------

void set_log_level(MpLogLevel level) noexcept;
void write_log(MpLogLevel level, const char* message) noexcept;
void logf(MpLogLevel level, const char* format, ...) noexcept;

/// The host vtable handed to every module. Its lifetime is the process.
[[nodiscard]] const MpHost& host_vtable() noexcept;

// --------------------------------------------------------------------------
// COM
// --------------------------------------------------------------------------

/// Multithreaded apartment for the calling thread, undone on destruction.
///
/// MTA rather than STA because the engine has no message pump and no UI, and
/// because the render thread has to be able to call the sink's COM interfaces
/// without marshalling on a 3 ms budget.
class ComApartment {
public:
    ComApartment();
    ~ComApartment();
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
    bool owned_ = false;
};

// --------------------------------------------------------------------------
// The render thread
// --------------------------------------------------------------------------

/// Joins MMCSS `Pro Audio` and the COM apartment, on the render thread itself.
///
/// Both have to happen there rather than on the thread that created it: MMCSS
/// registration is per-thread, and a thread that calls COM interfaces without
/// having entered an apartment is relying on undefined behaviour that usually
/// works.
class RenderThreadHooks final : public IRenderThreadHooks {
public:
    /// `pending` is a real answer and has to be one.
    ///
    /// `enter` runs on the render thread, and the thread that started the graph
    /// reaches the next line before that has necessarily happened. Reporting
    /// "MMCSS refused" from an uninitialised field is not a diagnostic, it is a
    /// race that reads as a system problem -- which cost an afternoon looking at
    /// a service that was running the whole time.
    enum class Realtime : std::uint32_t { pending = 0, granted = 1, refused = 2 };

    void enter() noexcept override;
    void leave() noexcept override;

    [[nodiscard]] Realtime realtime() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    /// GetLastError from the refusal, so a real one says why.
    [[nodiscard]] unsigned long refusal_code() const noexcept
    {
        return error_.load(std::memory_order_relaxed);
    }

    /// Blocks until the render thread has answered, or the deadline passes.
    /// Returns `pending` if it never did.
    Realtime wait_for_answer(unsigned milliseconds) const noexcept;

private:
    void* task_ = nullptr;
    bool com_owned_ = false;
    std::atomic<Realtime> state_{Realtime::pending};
    std::atomic<unsigned long> error_{0};
};

// --------------------------------------------------------------------------
// Module loading
// --------------------------------------------------------------------------

/// One loaded module, and the only code in the tree that calls LoadLibrary.
class LoadedModule {
public:
    /// Returns nullptr when the file is missing, has no entry point, or the
    /// entry point refuses this host's ABI version -- all three of which are
    /// normal answers rather than failures to report loudly.
    static std::unique_ptr<LoadedModule> load(const std::filesystem::path& path);

    ~LoadedModule();
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    LoadedModule(LoadedModule&&) = delete;
    LoadedModule& operator=(LoadedModule&&) = delete;

    [[nodiscard]] const MpModuleDesc& desc() const noexcept { return *desc_; }

    /// The sink vtable, or nullptr when this module is not a sink.
    [[nodiscard]] const MpSinkVtbl* sink_vtbl() const noexcept;

private:
    LoadedModule(void* handle, const MpModuleDesc* desc) noexcept
        : handle_(handle), desc_(desc)
    {
    }

    void* handle_ = nullptr;
    const MpModuleDesc* desc_ = nullptr;
};

/// Where modules live: beside the executable, as in DragonPerch.
[[nodiscard]] std::filesystem::path module_directory();

} // namespace mp::win

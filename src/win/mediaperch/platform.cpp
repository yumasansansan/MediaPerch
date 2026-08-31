// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/platform.hpp"

#include "mediaperch/win_headers.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mp::win {
namespace {

std::atomic<MpLogLevel> g_level{MP_LOG_INFO};

const char* level_name(MpLogLevel level) noexcept
{
    switch (level) {
    case MP_LOG_ERROR: return "error";
    case MP_LOG_WARN: return "warn ";
    case MP_LOG_INFO: return "info ";
    case MP_LOG_DEBUG: return "debug";
    default: return "?    ";
    }
}

void MP_CALL host_log(void*, MpLogLevel level, const char* message) noexcept
{
    write_log(level, message);
}

void* MP_CALL host_alloc(void*, std::size_t bytes) noexcept
{
    return std::malloc(bytes);
}

void MP_CALL host_release(void*, void* p) noexcept
{
    std::free(p);
}

const MpHost g_host = {
    /* size     */ sizeof(MpHost),
    /* reserved */ 0,
    /* ctx      */ nullptr,
    /* log      */ &host_log,
    /* alloc    */ &host_alloc,
    /* release  */ &host_release,
};

} // namespace

void set_log_level(MpLogLevel level) noexcept
{
    g_level.store(level, std::memory_order_relaxed);
}

void write_log(MpLogLevel level, const char* message) noexcept
{
    if (level > g_level.load(std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr, "[%s] %s\n", level_name(level), message);
}

void logf(MpLogLevel level, const char* format, ...) noexcept
{
    if (level > g_level.load(std::memory_order_relaxed)) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    write_log(level, buffer);
}

const MpHost& host_vtable() noexcept
{
    return g_host;
}

// --------------------------------------------------------------------------

ComApartment::ComApartment()
{
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means somebody already made this thread an STA. That is
    // their apartment, not ours to uninitialise, and COM still works.
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    owned_ = SUCCEEDED(hr);
}

ComApartment::~ComApartment()
{
    if (owned_) {
        ::CoUninitialize();
    }
}

// --------------------------------------------------------------------------

void RenderThreadHooks::enter() noexcept
{
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_owned_ = SUCCEEDED(hr);

    DWORD index = 0;
    task_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (task_ == nullptr) {
        error_.store(::GetLastError(), std::memory_order_relaxed);
        state_.store(Realtime::refused, std::memory_order_release);
        logf(MP_LOG_WARN, "MMCSS refused Pro Audio (error %lu)", ::GetLastError());
    } else {
        state_.store(Realtime::granted, std::memory_order_release);
    }
}

RenderThreadHooks::Realtime RenderThreadHooks::wait_for_answer(unsigned milliseconds)
    const noexcept
{
    for (unsigned waited = 0; waited <= milliseconds; waited += 1) {
        const Realtime state = realtime();
        if (state != Realtime::pending) {
            return state;
        }
        ::Sleep(1);
    }
    return realtime();
}

void RenderThreadHooks::leave() noexcept
{
    if (task_ != nullptr) {
        ::AvRevertMmThreadCharacteristics(task_);
        task_ = nullptr;
    }
    if (com_owned_) {
        ::CoUninitialize();
        com_owned_ = false;
    }
    state_.store(Realtime::pending, std::memory_order_release);
}

// --------------------------------------------------------------------------

std::unique_ptr<LoadedModule> LoadedModule::load(const std::filesystem::path& path)
{
    HMODULE handle = ::LoadLibraryExW(path.c_str(), nullptr,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
    if (handle == nullptr) {
        logf(MP_LOG_DEBUG, "no module at %s (error %lu)", path.string().c_str(),
             ::GetLastError());
        return nullptr;
    }

    auto entry = reinterpret_cast<MpModuleEntry>(
        reinterpret_cast<void*>(::GetProcAddress(handle, MP_MODULE_ENTRY_NAME)));
    if (entry == nullptr) {
        logf(MP_LOG_WARN, "%s has no " MP_MODULE_ENTRY_NAME, path.string().c_str());
        ::FreeLibrary(handle);
        return nullptr;
    }

    const MpModuleDesc* desc = entry(MP_ABI_VERSION);
    if (desc == nullptr) {
        logf(MP_LOG_WARN, "%s refused ABI version %u", path.string().c_str(),
             MP_ABI_VERSION);
        ::FreeLibrary(handle);
        return nullptr;
    }
    if (desc->size < sizeof(MpModuleDesc) || desc->abi_version != MP_ABI_VERSION) {
        logf(MP_LOG_WARN, "%s returned a descriptor this host cannot read",
             path.string().c_str());
        ::FreeLibrary(handle);
        return nullptr;
    }

    if (desc->init != nullptr) {
        const MpResult r = desc->init(&host_vtable());
        if (r != MP_OK) {
            logf(MP_LOG_WARN, "%s failed to initialise (%u)", desc->id, r);
            ::FreeLibrary(handle);
            return nullptr;
        }
    }

    logf(MP_LOG_DEBUG, "loaded %s (%s)", desc->id, desc->name);
    return std::unique_ptr<LoadedModule>(new LoadedModule(handle, desc));
}

LoadedModule::~LoadedModule()
{
    if (desc_ != nullptr && desc_->shutdown != nullptr) {
        desc_->shutdown();
    }
    if (handle_ != nullptr) {
        // A module that started threads or registered COM classes says so, and
        // is honestly leaked for the process lifetime rather than dishonestly
        // unloaded out from under whatever still points into it.
        if (desc_ == nullptr || (desc_->flags & MP_MODULE_NO_UNLOAD) == 0) {
            ::FreeLibrary(static_cast<HMODULE>(handle_));
        }
        handle_ = nullptr;
    }
}

const MpSinkVtbl* LoadedModule::sink_vtbl() const noexcept
{
    if (desc_ == nullptr || desc_->kind != MP_KIND_SINK) {
        return nullptr;
    }
    const auto* vtbl = static_cast<const MpSinkVtbl*>(desc_->vtbl);
    if (vtbl == nullptr || vtbl->size < sizeof(MpSinkVtbl)) {
        return nullptr;
    }
    return vtbl;
}

std::filesystem::path module_directory()
{
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path{buffer}.parent_path();
}

} // namespace mp::win

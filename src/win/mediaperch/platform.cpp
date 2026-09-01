// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/platform.hpp"

#include "mediaperch/win_headers.hpp"

#include <algorithm>
#include <array>
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

void ModuleRegistry::scan(const std::filesystem::path& directory)
{
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        const auto& path = entry.path();
        if (path.extension() != ".dll") {
            continue;
        }
        const std::string name = path.filename().string();
        if (name.rfind("mp_", 0) != 0) {
            continue;
        }
        candidates.push_back(path);
    }
    // Deterministic order, so two runs on one machine agree about ties.
    std::sort(candidates.begin(), candidates.end());

    for (const auto& path : candidates) {
        if (auto module = LoadedModule::load(path)) {
            modules_.push_back(std::move(module));
        }
    }
}

std::vector<const MpModuleDesc*> ModuleRegistry::all() const
{
    std::vector<const MpModuleDesc*> out;
    out.reserve(modules_.size());
    for (const auto& module : modules_) {
        out.push_back(&module->desc());
    }
    return out;
}

const MpDspVtbl* ModuleRegistry::dsp(std::string_view id) const
{
    for (const auto& module : modules_) {
        const MpModuleDesc& desc = module->desc();
        if (desc.kind != MP_KIND_DSP || id != desc.id) {
            continue;
        }
        const auto* vtbl = static_cast<const MpDspVtbl*>(desc.vtbl);
        return (vtbl != nullptr && vtbl->size >= sizeof(MpDspVtbl)) ? vtbl : nullptr;
    }
    return nullptr;
}

std::vector<const MpModuleDesc*> ModuleRegistry::dsps() const
{
    std::vector<const MpModuleDesc*> out;
    for (const auto& module : modules_) {
        if (module->desc().kind == MP_KIND_DSP) {
            out.push_back(&module->desc());
        }
    }
    return out;
}

const MpSinkVtbl* ModuleRegistry::sink(std::string_view id) const
{
    const MpSinkVtbl* best = nullptr;
    std::uint32_t best_priority = 0;
    for (const auto& module : modules_) {
        const MpSinkVtbl* vtbl = module->sink_vtbl();
        if (vtbl == nullptr) {
            continue;
        }
        if (!id.empty()) {
            if (id == module->desc().id) {
                return vtbl;
            }
            continue;
        }
        if (best == nullptr || module->desc().priority > best_priority) {
            best = vtbl;
            best_priority = module->desc().priority;
        }
    }
    return id.empty() ? best : nullptr;
}

ModuleRegistry::DecoderChoice ModuleRegistry::decoder_for(const std::string& path,
                                                          std::string_view prefer) const
{
    const auto ranked = decoders_for(path, prefer);
    return ranked.empty() ? DecoderChoice{} : ranked.front();
}

std::vector<ModuleRegistry::DecoderChoice> ModuleRegistry::decoders_for(
    const std::string& path, std::string_view prefer) const
{
    // The first few kilobytes, which is all a probe is allowed to look at. A
    // probe that opens the file has already done the expensive thing twice.
    std::array<std::uint8_t, 4096> head{};
    std::size_t head_bytes = 0;
    if (std::FILE* file = open_utf8(path, L"rb")) {
        head_bytes = std::fread(head.data(), 1, head.size(), file);
        std::fclose(file);
    }

    std::vector<DecoderChoice> ranked;

    for (const auto& module : modules_) {
        const MpModuleDesc& desc = module->desc();
        if (desc.kind != MP_KIND_DECODER) {
            continue;
        }
        const auto* vtbl = static_cast<const MpDecoderVtbl*>(desc.vtbl);
        if (vtbl == nullptr || vtbl->size < sizeof(MpDecoderVtbl)) {
            continue;
        }

        if (!prefer.empty()) {
            // An explicit choice is a choice, not a preference: it does not get
            // a fallback, because "use that one" answered with a different one
            // is not an answer.
            if (prefer == desc.id) {
                return {DecoderChoice{vtbl, &desc, 0}};
            }
            continue;
        }

        std::uint32_t score = 0;
        if (vtbl->probe != nullptr) {
            vtbl->probe(path.c_str(), head.data(), head_bytes, &score);
        }
        if (score == 0) {
            continue;
        }
        ranked.push_back(DecoderChoice{vtbl, &desc, score});
    }

    // Score first, priority to break ties -- the same rule as before, now
    // applied to the whole list rather than just its maximum. stable_sort so
    // that two modules agreeing on both keep the order the registry loaded them
    // in, which is at least deterministic.
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const DecoderChoice& a, const DecoderChoice& b) {
                         if (a.score != b.score) {
                             return a.score > b.score;
                         }
                         return a.desc->priority > b.desc->priority;
                     });
    return ranked;
}

std::FILE* open_utf8(const std::string& path, const wchar_t* mode) noexcept
{
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide <= 0) {
        return nullptr;
    }
    std::vector<wchar_t> name(static_cast<std::size_t>(wide));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, name.data(), wide);
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, name.data(), mode) != 0) {
        return nullptr;
    }
    return file;
}

std::vector<std::string> command_line_utf8()
{
    std::vector<std::string> out;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) {
            out.emplace_back();
            continue;
        }
        std::string narrow(static_cast<std::size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, narrow.data(), bytes, nullptr, nullptr);
        out.push_back(std::move(narrow));
    }
    LocalFree(wide);
    return out;
}

ConsoleUtf8::ConsoleUtf8() noexcept : previous_(GetConsoleOutputCP())
{
    if (previous_ != CP_UTF8) {
        SetConsoleOutputCP(CP_UTF8);
    }
}

ConsoleUtf8::~ConsoleUtf8()
{
    // Put it back: the code page belongs to the console window, not to this
    // process, and leaving it changed outlives the program.
    if (previous_ != 0 && previous_ != CP_UTF8) {
        SetConsoleOutputCP(previous_);
    }
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

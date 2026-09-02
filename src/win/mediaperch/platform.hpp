// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Windows head: everything the portable core is handed rather than knows.

#include "mediaperch/source.hpp"

#include <mediaperch/module.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// `fopen` for a UTF-8 path, which the narrow CRT cannot do.
///
/// The narrow calls go through the process code page, so half the files on a
/// Japanese or Russian machine cannot be opened by name. The wide call can, and
/// unlike `std::ifstream` it brings no iostreams, no locale facets and no
/// `std::filesystem::path` conversion with it -- the whole of which is several
/// kilobytes in every binary that touches a file.
[[nodiscard]] std::FILE* open_utf8(const std::string& path, const wchar_t* mode) noexcept;

/// The command line as UTF-8, taken from the wide one Windows really has.
///
/// **`main`'s `argv` is in the process code page, and the ABI says paths are
/// UTF-8.** On a machine whose code page is 932 those are not the same thing,
/// and every file whose name is not ASCII arrives at a decoder as bytes that
/// are not the name of anything -- which looks exactly like "no decoder
/// recognised this file". The wide command line is the only one that was never
/// lossy, so it is the one this reads.
[[nodiscard]] std::vector<std::string> command_line_utf8();

/// Makes the console speak UTF-8 for as long as it exists, and puts it back.
///
/// Without it a UTF-8 filename in the report is mojibake, which turns "the
/// player cannot open this file" and "the player cannot print its name" into
/// the same picture.
class ConsoleUtf8 {
public:
    ConsoleUtf8() noexcept;
    ~ConsoleUtf8();
    ConsoleUtf8(const ConsoleUtf8&) = delete;
    ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;
    ConsoleUtf8(ConsoleUtf8&&) = delete;
    ConsoleUtf8& operator=(ConsoleUtf8&&) = delete;

private:
    unsigned int previous_ = 0;
};

/// Everything that loaded, and the rules for choosing between them.
///
/// This is §7 of the plan made real. Until it existed the tool named the decoder
/// DLL it wanted, which works for exactly as long as there is one.
class ModuleRegistry {
public:
    ModuleRegistry() = default;
    ~ModuleRegistry() = default;
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;
    ModuleRegistry(ModuleRegistry&&) = delete;
    ModuleRegistry& operator=(ModuleRegistry&&) = delete;

    /// Loads every `mp_*.dll` beside the executable that this host can read.
    /// A module that refuses the ABI version, or returns a descriptor from the
    /// future, is skipped with a line in the log rather than a failure.
    ///
    /// `allow`, when it is not empty, is the module ids that may be loaded --
    /// and it is checked *after* loading, because a module's id is inside it.
    /// A module that is not on the list is unloaded again immediately, which is
    /// the most an allow-list can promise when the name on disk is not the
    /// name in the descriptor.
    void scan(const std::filesystem::path& directory,
              const std::vector<std::string>& allow = {});

    [[nodiscard]] std::vector<const MpModuleDesc*> all() const;

    /// `id` empty means "the highest-priority one".
    [[nodiscard]] const MpSinkVtbl* sink(std::string_view id = {}) const;

    /// A DSP stage by module id, or nullptr. Path B's chain is built from
    /// these, and a stage nobody had written when this was compiled is found
    /// the same way as one that ships with it.
    [[nodiscard]] const MpDspVtbl* dsp(std::string_view id) const;
    /// Every loaded DSP module, for `--dsp list` and for a settings dialogue.
    [[nodiscard]] std::vector<const MpModuleDesc*> dsps() const;

    struct DemuxChoice {
        const MpDemuxVtbl* vtbl = nullptr;
        const MpModuleDesc* desc = nullptr;
        std::uint32_t score = 0;
    };

    /// Every demuxer that claims this file's *container*, best first.
    ///
    /// One probe per module on the first four kilobytes, which is what magic
    /// bytes are for and all a container needs. What is inside it is not asked
    /// here and is not visible from four kilobytes anyway -- that is what
    /// opening the container is for.
    [[nodiscard]] std::vector<DemuxChoice> demuxers_for(const std::string& path,
                                                        std::string_view prefer = {}) const;

    /// Which module decodes `codec`. **Looked up, not tried.**
    ///
    /// A module that declared its codecs in its descriptor is filtered on that
    /// declaration first -- capability declaration is data, not code, and a
    /// module that named its codecs has promised. The rest are asked, and the
    /// best score wins with priority breaking ties, as everywhere else.
    [[nodiscard]] const MpCodecVtbl* codec_for(MpCodec codec, const std::uint8_t* config,
                                               std::uint32_t config_bytes) const;

    struct DecoderChoice {
        const MpDecoderVtbl* vtbl = nullptr;
        const MpModuleDesc* desc = nullptr;
        std::uint32_t score = 0;
    };

    /// An explicit `prefer` wins outright, even over a decoder that would score
    /// higher -- being able to say "use that one" is the point of having more
    /// than one. Otherwise every decoder is shown the file's first bytes and the
    /// best score wins, ties broken by the module's declared priority.
    [[nodiscard]] DecoderChoice decoder_for(const std::string& path,
                                            std::string_view prefer) const;

    /// Every decoder that claims the file, best first.
    ///
    /// A probe sees four kilobytes; opening sees the whole header and, through
    /// mp::Decoder, one frame of real audio. A decoder can therefore score
    /// highest and still refuse -- decode_mf declines multichannel ALAC because
    /// Media Foundation returns it in the wrong channel order, decode_native
    /// decodes a 32-bit FLAC to nothing -- and when that happens the answer is
    /// the next candidate, not failure. Ranking without a fallback turns every
    /// such refusal into an unplayable file.
    [[nodiscard]] std::vector<DecoderChoice> decoders_for(const std::string& path,
                                                          std::string_view prefer) const;

private:
    std::vector<std::unique_ptr<LoadedModule>> modules_;
};

} // namespace mp::win

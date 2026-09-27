// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bytes, as a file a module can open by name -- fast enough to do it for every
// input a fuzzer makes, and for every broken copy of a file a test makes.
//
// **A module opens a path**, so the bytes have to be a file, and on Windows a
// file written with WriteFile is scanned for malware when it is next opened:
// 7.5 to 8.8 ms, measured, where the module's own open and read took 0.04. A
// file whose bytes were copied in through a mapped view is not scanned, and a
// mapping cannot change a file's size, so each size gets a file of its own:
// written the first time it is seen, and scanned that once, and copied into
// through a mapping after that -- 0.09 ms. The mapping is closed again before
// the module opens the file, because the modules open with `_wfopen_s`, whose
// share mode refuses a file anybody else has open for writing.
//
// Past a budget of bytes the files are all removed and the count starts
// again, and they are removed with the object. A process that dies -- which is
// what a fuzzer that finds something does -- leaves its files, and the next one
// made with the same prefix removes those of any process no longer running.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>

#    include <process.h>
#else
#    include <signal.h>
#    include <unistd.h>
#endif

namespace mp::test {

class ScratchFiles {
public:
    /// Files named `<prefix><process id>-<size>` in the temporary directory.
    explicit ScratchFiles(std::string_view prefix) : prefix_(prefix)
    {
#ifdef _WIN32
        const auto id = static_cast<unsigned long>(_getpid());
#else
        const auto id = static_cast<unsigned long>(getpid());
#endif
        const std::filesystem::path temp = std::filesystem::temp_directory_path();
        base_ = temp / (prefix_ + std::to_string(id) + "-");
        remove_orphans(temp);
    }
    ~ScratchFiles() { clear(); }
    ScratchFiles(const ScratchFiles&) = delete;
    ScratchFiles& operator=(const ScratchFiles&) = delete;

    /// The UTF-8 path of a file holding exactly these bytes, or null. Valid
    /// until the next call.
    [[nodiscard]] const char* hold(const std::uint8_t* data, std::size_t size)
    {
        if (held_ > k_budget) {
            clear();
        }
        const auto found = files_.find(size);
        if (found != files_.end()) {
            return copy_in(found->second, data, size) ? found->second.utf8.c_str() : nullptr;
        }
        File file;
        std::filesystem::path path = base_;
        path += std::to_string(size);
        file.native = path;
        const std::u8string utf8 = path.u8string();
        file.utf8.assign(utf8.begin(), utf8.end());
        if (!make(file, data, size)) {
            std::filesystem::remove(file.native, ignored_);
            return nullptr;
        }
        held_ += size;
        return files_.emplace(size, std::move(file)).first->second.utf8.c_str();
    }

private:
    static constexpr std::size_t k_budget = std::size_t{256} << 20;

    struct File {
        std::filesystem::path native;
        std::string utf8;
    };

    /// The first time a size is seen: written, as any file is.
    static bool make(const File& file, const std::uint8_t* data, std::size_t size)
    {
        std::FILE* out = nullptr;
#ifdef _WIN32
        if (::_wfopen_s(&out, file.native.c_str(), L"wb") != 0) {
            return false;
        }
#else
        out = std::fopen(file.native.c_str(), "wb");
#endif
        if (out == nullptr) {
            return false;
        }
        const bool written = size == 0 || std::fwrite(data, 1, size, out) == size;
        return std::fclose(out) == 0 && written;
    }

    /// Every time after: through a mapping on Windows, which is not scanned.
    static bool copy_in(const File& file, const std::uint8_t* data, std::size_t size)
    {
        if (size == 0) {
            return true;
        }
#ifdef _WIN32
        const HANDLE handle = ::CreateFileW(file.native.c_str(), GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        const HANDLE mapping = ::CreateFileMappingW(handle, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        void* const view =
            mapping != nullptr ? ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : nullptr;
        if (view != nullptr) {
            std::memcpy(view, data, size);
            ::UnmapViewOfFile(view);
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
        }
        ::CloseHandle(handle);
        return view != nullptr;
#else
        return make(file, data, size);
#endif
    }

    /// The files of processes that died, which no destructor removed.
    void remove_orphans(const std::filesystem::path& temp)
    {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator{temp, error}) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix_, 0) != 0) {
                continue;
            }
            const std::string rest = name.substr(prefix_.size());
            const unsigned long id = std::strtoul(rest.c_str(), nullptr, 10);
            if (id != 0 && !running(id)) {
                std::filesystem::remove(entry.path(), error);
            }
        }
    }

    static bool running(unsigned long id)
    {
#ifdef _WIN32
        const HANDLE process =
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
        if (process == nullptr) {
            return false;
        }
        DWORD code = 0;
        const bool alive = ::GetExitCodeProcess(process, &code) != FALSE && code == STILL_ACTIVE;
        ::CloseHandle(process);
        return alive;
#else
        return ::kill(static_cast<pid_t>(id), 0) == 0;
#endif
    }

    void clear()
    {
        for (const auto& [size, file] : files_) {
            std::filesystem::remove(file.native, ignored_);
        }
        files_.clear();
        held_ = 0;
    }

    std::string prefix_;
    std::filesystem::path base_;
    std::unordered_map<std::size_t, File> files_;
    std::size_t held_ = 0;
    std::error_code ignored_;
};

} // namespace mp::test

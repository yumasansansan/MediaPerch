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
// the module opens the file, because the modules open with `_wfopen_s` there
// (modules/shared/module_log/module_file_windows.cpp), whose share mode refuses
// a file anybody else has open for writing.
//
// Past a budget of bytes the files are all removed and the count starts
// again, and they are removed with the object. A process that dies -- which is
// what a fuzzer that finds something does -- leaves its files, and the next one
// made with the same prefix removes those of any process no longer running.
//
// The mapping and the questions about processes are the system's, and are
// test_platform.hpp's to answer, per system and chosen by the build; this is
// ISO C++.

#include "test_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace mp::test {

class ScratchFiles {
public:
    /// Files named `<prefix><process id>-<size>` in the temporary directory.
    explicit ScratchFiles(std::string_view prefix) : prefix_(prefix)
    {
        const unsigned long id = platform::process_id();
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

    /// The first time a size is seen: written, as any file is. std::ofstream
    /// takes the path as the system spells it, UTF-16 on Windows included.
    static bool make(const File& file, const std::uint8_t* data, std::size_t size)
    {
        std::ofstream out{file.native, std::ios::binary | std::ios::trunc};
        if (!out) {
            return false;
        }
        if (size != 0) {
            write_bytes(out, data, size);
        }
        out.close();
        return !out.fail();
    }

    /// Every time after: through a mapping on Windows, which is not scanned.
    static bool copy_in(const File& file, const std::uint8_t* data, std::size_t size)
    {
        return size == 0 || platform::overwrite(file.native, data, size);
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

    static bool running(unsigned long id) { return platform::process_running(id); }

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

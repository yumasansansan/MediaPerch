// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files past four gibibytes, read where their bytes lie.
//
// **An offset or a count held in 32 bits is the fault no small file shows**,
// and every fixture here is under a megabyte -- while an evening of 24-bit
// 192 kHz stereo is past four gibibytes, and a film is past it at once. These
// make files of twenty gibibytes that take 128 KiB of disk: sparse, on NTFS,
// with nothing written between the header and the last bytes, which a read
// gets back as zeros. The demuxer is asked for what is at the end: a count cut
// to 32 bits puts the end somewhere else, and an offset cut to 32 bits reads
// from somewhere else, and either way the bytes that come back are not the
// ones written there.

#include "module_loader.hpp"
#include "temp_path.hpp"

#include <mediaperch/module.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

namespace {

constexpr std::uint64_t k_gib = std::uint64_t{1} << 30;

/// A sparse file of a given size: `head` at its start, `tail` at `tail_at`, and
/// nothing on disk between them.
class SparseFile {
public:
    explicit SparseFile(std::string_view name) : path_(mp::test::temp_path(name)) {}
    ~SparseFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    SparseFile(const SparseFile&) = delete;
    SparseFile& operator=(const SparseFile&) = delete;

    /// False when the file system cannot make it sparse, which is a volume
    /// this test is not going to fill with twenty gibibytes of zeros.
    [[nodiscard]] bool write(const std::vector<std::uint8_t>& head, std::uint64_t tail_at,
                             const std::vector<std::uint8_t>& tail) const
    {
        const HANDLE file = ::CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD returned = 0;
        bool ok = ::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                                    nullptr) != FALSE;
        DWORD written = 0;
        ok = ok && ::WriteFile(file, head.data(), static_cast<DWORD>(head.size()), &written,
                               nullptr) != FALSE;
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(tail_at);
        ok = ok && ::SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE;
        ok = ok && ::WriteFile(file, tail.data(), static_cast<DWORD>(tail.size()), &written,
                               nullptr) != FALSE;
        ::CloseHandle(file);
        return ok;
    }

    [[nodiscard]] std::string utf8() const
    {
        const std::u8string text = path_.u8string();
        return {text.begin(), text.end()};
    }

private:
    std::filesystem::path path_;
};

/// Bytes nothing else would put at the end of a file.
std::vector<std::uint8_t> pattern(std::size_t bytes)
{
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    return out;
}

void put_le(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

void put_text(std::vector<std::uint8_t>& out, std::string_view text)
{
    out.insert(out.end(), text.begin(), text.end());
}

// EBML, written the long way: every size in eight bytes, which is legal and
// is one less thing to get wrong.

void put_size(std::vector<std::uint8_t>& out, std::uint64_t size)
{
    out.push_back(0x01);
    for (int i = 6; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((size >> (8 * i)) & 0xFFu));
    }
}

void element(std::vector<std::uint8_t>& out, std::initializer_list<std::uint8_t> id,
             const std::vector<std::uint8_t>& payload)
{
    out.insert(out.end(), id.begin(), id.end());
    put_size(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> number(std::uint64_t value)
{
    std::vector<std::uint8_t> out;
    int bytes = 1;
    while (bytes < 8 && (value >> (8 * bytes)) != 0) {
        ++bytes;
    }
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
    return out;
}

std::vector<std::uint8_t> real(double value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    std::vector<std::uint8_t> out;
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
    }
    return out;
}

std::vector<std::uint8_t> text(std::string_view value)
{
    return {value.begin(), value.end()};
}

/// Every byte of every packet from where the demuxer is now to the end.
std::vector<std::uint8_t> read_to_end(const MpDemuxVtbl& v, MpDemux* d, std::uint64_t& first)
{
    std::vector<std::uint8_t> all;
    std::vector<std::uint8_t> buffer(1u << 16);
    first = ~std::uint64_t{0};
    for (int i = 0; i < 4096; ++i) {
        MpPacket packet{};
        packet.size = sizeof(packet);
        MpResult r = v.read_packet(d, buffer.data(), buffer.size(), &packet);
        if (r == MP_ERR_NO_MEMORY && packet.bytes <= (1u << 26)) {
            buffer.resize(packet.bytes);
            packet = MpPacket{};
            packet.size = sizeof(packet);
            r = v.read_packet(d, buffer.data(), buffer.size(), &packet);
        }
        if (r != MP_OK) {
            break;
        }
        if (first == ~std::uint64_t{0}) {
            first = packet.frame;
        }
        all.insert(all.end(), buffer.begin(), buffer.begin() + packet.bytes);
    }
    return all;
}

} // namespace

TEST_CASE("a WAV of twenty gibibytes is counted, and read at its end", "[demux][huge]")
{
    // RF64, which is how WAV gets past four gibibytes: the 32-bit sizes say
    // 0xFFFFFFFF and a ds64 chunk holds the real ones. Sixteen-bit stereo, so
    // twenty gibibytes is 5,368,709,120 frames -- past 2^32 as a count, and
    // past it as an offset.
    const std::uint64_t data_bytes = 20 * k_gib;
    const std::uint64_t frames = data_bytes / 4;
    std::vector<std::uint8_t> head;
    put_text(head, "RF64");
    put_le(head, 0xFFFFFFFFu, 4);
    put_text(head, "WAVE");
    put_text(head, "ds64");
    put_le(head, 28, 4);
    const std::uint64_t header_bytes = 12 + 36 + 24 + 8;
    put_le(head, header_bytes + data_bytes - 8, 8); // riffSize
    put_le(head, data_bytes, 8);                     // dataSize
    put_le(head, frames, 8);                         // sampleCount
    put_le(head, 0, 4);                              // table length
    put_text(head, "fmt ");
    put_le(head, 16, 4);
    put_le(head, 1, 2); // PCM
    put_le(head, 2, 2);
    put_le(head, 44100, 4);
    put_le(head, 44100 * 4, 4);
    put_le(head, 4, 2);
    put_le(head, 16, 2);
    put_text(head, "data");
    put_le(head, 0xFFFFFFFFu, 4);
    REQUIRE(head.size() == header_bytes);

    // The last 64 frames.
    const std::vector<std::uint8_t> tail = pattern(256);
    SparseFile file{"huge.wav"};
    if (!file.write(head, header_bytes + data_bytes - tail.size(), tail)) {
        SKIP("the temporary directory's volume will not make a sparse file");
    }

    mp::test::Module module{MEDIAPERCH_DEMUX_WAV, MP_KIND_DEMUX};
    const MpDemuxVtbl* v = module.as<MpDemuxVtbl>();
    REQUIRE(v != nullptr);
    MpDemux* d = nullptr;
    const std::string path = file.utf8();
    REQUIRE(v->open(path.c_str(), &d) == MP_OK);
    MpStreamInfo info{};
    info.size = sizeof(info);
    REQUIRE(v->stream_info(d, 0, &info) == MP_OK);
    CHECK(info.total_frames == frames);

    const std::uint32_t only = 0;
    REQUIRE(v->select_streams(d, &only, 1) == MP_OK);
    REQUIRE(v->seek(d, 0, frames - 64) == MP_OK);
    std::uint64_t first = 0;
    const std::vector<std::uint8_t> end = read_to_end(*v, d, first);
    CHECK(first == frames - 64);
    REQUIRE(end.size() >= tail.size());
    CHECK(std::vector<std::uint8_t>(end.end() - static_cast<std::ptrdiff_t>(tail.size()),
                                    end.end()) == tail);
    v->close(d);
}

TEST_CASE("a Matroska cluster past four gibibytes is found, and read", "[demux][huge]")
{
    // A segment of unknown size -- which is what a recording still being
    // written has -- holding the header elements, five gibibytes of Void, and
    // the one cluster. The demuxer walks the segment for its index; the walk
    // has to step over the Void by seeking, and land where the cluster is.
    std::vector<std::uint8_t> head;
    element(head, {0x1A, 0x45, 0xDF, 0xA3}, [] {
        std::vector<std::uint8_t> ebml;
        element(ebml, {0x42, 0x86}, number(1));
        element(ebml, {0x42, 0xF7}, number(1));
        element(ebml, {0x42, 0xF2}, number(4));
        element(ebml, {0x42, 0xF3}, number(8));
        element(ebml, {0x42, 0x82}, text("matroska"));
        element(ebml, {0x42, 0x87}, number(4));
        element(ebml, {0x42, 0x85}, number(2));
        return ebml;
    }());
    // The segment: its ID and a size of all ones, which is "unknown".
    head.insert(head.end(), {0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    element(head, {0x15, 0x49, 0xA9, 0x66}, [] {
        std::vector<std::uint8_t> info;
        element(info, {0x2A, 0xD7, 0xB1}, number(1000000));
        return info;
    }());
    element(head, {0x16, 0x54, 0xAE, 0x6B}, [] {
        std::vector<std::uint8_t> entry;
        element(entry, {0xD7}, number(1));
        element(entry, {0x73, 0xC5}, number(1));
        element(entry, {0x83}, number(2)); // audio
        element(entry, {0x86}, text("A_PCM/INT/LIT"));
        element(entry, {0xE1}, [] {
            std::vector<std::uint8_t> audio;
            element(audio, {0xB5}, real(44100.0));
            element(audio, {0x9F}, number(2));
            element(audio, {0x62, 0x64}, number(16));
            return audio;
        }());
        std::vector<std::uint8_t> tracks;
        element(tracks, {0xAE}, entry);
        return tracks;
    }());
    // Four kibibytes first: the same file with nothing large in it, so that a
    // failure of the large one is a failure of its size and not of its shape.
    std::uint64_t void_bytes = 0;
    SECTION("past four kibibytes of Void")
    {
        void_bytes = 4096;
    }
    SECTION("past five gibibytes of Void")
    {
        void_bytes = 5 * k_gib;
    }
    head.push_back(0xEC);
    put_size(head, void_bytes);

    const std::vector<std::uint8_t> audio = pattern(256);
    std::vector<std::uint8_t> cluster;
    element(cluster, {0x1F, 0x43, 0xB6, 0x75}, [&] {
        std::vector<std::uint8_t> inside;
        element(inside, {0xE7}, number(0));
        std::vector<std::uint8_t> block{0x81, 0x00, 0x00, 0x80}; // track 1, at 0, a keyframe
        block.insert(block.end(), audio.begin(), audio.end());
        element(inside, {0xA3}, block);
        return inside;
    }());

    SparseFile file{"huge.mkv"};
    if (!file.write(head, head.size() + void_bytes, cluster)) {
        SKIP("the temporary directory's volume will not make a sparse file");
    }

    mp::test::Module module{MEDIAPERCH_DEMUX_MKV, MP_KIND_DEMUX};
    const MpDemuxVtbl* v = module.as<MpDemuxVtbl>();
    REQUIRE(v != nullptr);
    MpDemux* d = nullptr;
    const std::string path = file.utf8();
    REQUIRE(v->open(path.c_str(), &d) == MP_OK);
    std::uint32_t streams = 0;
    REQUIRE(v->stream_count(d, &streams) == MP_OK);
    REQUIRE(streams == 1u);
    const std::uint32_t only = 0;
    REQUIRE(v->select_streams(d, &only, 1) == MP_OK);
    std::uint64_t first = 0;
    CHECK(read_to_end(*v, d, first) == audio);
    v->close(d);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// An audio codec module, through the module ABI, given arbitrary packets.
//
// **What a demuxer hands a codec is the file's bytes too**: a packet is cut out
// of the container and passed on verbatim, and so is the configuration blob --
// a FLAC STREAMINFO, an OpusHead, Vorbis's three headers. The demuxer fuzzers
// reach a codec never; this reaches one directly, with the module's source and
// the library under it compiled in and instrumented, and drives it the way the
// host does: probe, open with the configuration, packets decoded, a reset in
// the middle, as after a seek, and a flush at the end. It holds the answers to
// what module.h promises -- no more written than the buffer holds, PCM out in
// the format get_format reports and so in whole frames of it, a null where a
// packet or an answer should be refused -- and a broken promise is a crash here,
// as a bad read is.
//
// The input: one byte choosing among the codecs the module names, two bytes of
// configuration length and the configuration, then packets, each two bytes of
// length and the bytes. The last byte also says which of libFLAC's allocations
// fail, in the fuzzer that links it (module_harness.hpp); a seed ends in a byte
// no packet takes, zero, so that none does.

#include "module_harness.hpp"

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using mp::fuzz::broken;

/// Room for any one packet's audio: FLAC's largest block, 65,535 frames of
/// eight channels of 32 bits, is two mebibytes.
constexpr std::size_t k_room = std::size_t{4} << 20;
constexpr int k_packets = 256;
constexpr int k_flushes = 8;

const MpModuleDesc& module()
{
    static const MpModuleDesc* const desc = [] {
        const MpModuleDesc* found = mp_module_entry(MP_ABI_VERSION);
        if (found == nullptr || found->kind != MP_KIND_CODEC || found->vtbl == nullptr ||
            found->codec_count == 0 || found->codecs == nullptr) {
            broken("mp_module_entry names a codec, and the codecs it decodes");
        }
        if (found->init != nullptr && found->init(&mp::fuzz::host) != MP_OK) {
            broken("init succeeds with a working host");
        }
        return found;
    }();
    return *desc;
}

/// Bytes a frame of this format takes, or zero for a format whose frame this
/// cannot say -- not yet known, or not PCM.
std::size_t frame_bytes(const MpFormat& format)
{
    if (format.encoding != MP_ENCODING_PCM || format.channels == 0) {
        return 0;
    }
    std::size_t sample = 0;
    switch (format.sample_type) {
    case MP_SAMPLE_U8:
        sample = 1;
        break;
    case MP_SAMPLE_S16:
        sample = 2;
        break;
    case MP_SAMPLE_S24_PACKED:
        sample = 3;
        break;
    case MP_SAMPLE_S24_IN_32:
    case MP_SAMPLE_S32:
    case MP_SAMPLE_F32:
        sample = 4;
        break;
    case MP_SAMPLE_F64:
        sample = 8;
        break;
    default:
        return 0;
    }
    return sample * format.channels;
}

/// Reads the input front to back; runs out quietly.
struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;

    std::size_t left() const { return size - at; }
    std::uint32_t u8() { return at < size ? data[at++] : 0u; }
    std::uint32_t u16() { return u8() | (u8() << 8); }
    const std::uint8_t* take(std::size_t bytes)
    {
        const std::uint8_t* start = data + at;
        at += bytes;
        return start;
    }
};

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    (void)module();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const MpModuleDesc& desc = module();
    const auto& v = *static_cast<const MpCodecVtbl*>(desc.vtbl);
    static std::vector<std::uint8_t> out(k_room);

    mp::fuzz::fail_flac_allocations(size != 0 ? data[size - 1] : 0);
    Reader in{data, size};
    const MpCodec codec = desc.codecs[in.u8() % desc.codec_count];
    std::size_t config_bytes = in.u16();
    config_bytes = config_bytes < in.left() ? config_bytes : in.left();
    const std::uint8_t* config = config_bytes != 0 ? in.take(config_bytes) : nullptr;
    const auto config_length = static_cast<std::uint32_t>(config_bytes);

    std::uint32_t score = 0;
    v.probe(codec, config, config_length, &score);
    if (score > 100) {
        broken("a probe scores 0 to 100");
    }
    if (v.probe(codec, config, config_length, nullptr) == MP_OK) {
        broken("probe refuses a null out_score");
    }

    MpCodecInstance* c = nullptr;
    if (v.open(codec, config, config_length, &c) != MP_OK) {
        return 0;
    }
    if (c == nullptr) {
        broken("open that succeeds hands back a decoder");
    }

    MpFormat format{};
    if (v.get_format(c, nullptr) == MP_OK) {
        broken("get_format refuses a null out");
    }
    std::size_t got = 0;
    const std::uint8_t sample[4] = {};
    if (v.decode(c, nullptr, sizeof(sample), out.data(), out.size(), &got) == MP_OK) {
        broken("decode refuses a null packet of four bytes");
    }
    if (v.decode(c, sample, sizeof(sample), out.data(), out.size(), nullptr) == MP_OK) {
        broken("decode refuses a null out_bytes");
    }

    // The packets, with a reset after the first half of them, as a seek makes.
    std::vector<std::pair<const std::uint8_t*, std::size_t>> packets;
    while (in.left() >= 2 && static_cast<int>(packets.size()) < k_packets) {
        std::size_t bytes = in.u16();
        bytes = bytes < in.left() ? bytes : in.left();
        if (bytes == 0) {
            continue;
        }
        packets.emplace_back(in.take(bytes), bytes);
    }
    for (std::size_t p = 0; p < packets.size(); ++p) {
        if (p == packets.size() / 2 && v.reset(c) != MP_OK) {
            // A codec that cannot reset is one a host closes; this one is done.
            break;
        }
        got = 0;
        if (v.decode(c, packets[p].first, packets[p].second, out.data(), out.size(), &got) !=
            MP_OK) {
            continue;
        }
        if (got > out.size()) {
            broken("decode writes no more than the buffer holds");
        }
        if (v.get_format(c, &format) == MP_OK) {
            const std::size_t frame = frame_bytes(format);
            if (frame != 0 && got % frame != 0) {
                broken("decode writes whole frames of the format get_format reports");
            }
        }
    }

    for (int f = 0; f < k_flushes; ++f) {
        got = 0;
        if (v.flush(c, out.data(), out.size(), &got) != MP_OK || got == 0) {
            break;
        }
        if (got > out.size()) {
            broken("flush writes no more than the buffer holds");
        }
    }
    if (v.flush(c, out.data(), out.size(), nullptr) == MP_OK) {
        broken("flush refuses a null out_bytes");
    }

    v.close(c);
    return 0;
}

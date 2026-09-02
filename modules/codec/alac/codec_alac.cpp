// SPDX-License-Identifier: GPL-3.0-or-later
//
// ALAC, as a codec and nothing else.
//
// It never sees a file. It is handed the `ALACSpecificConfig` the container
// found and then packets, one at a time, and it produces the file's own
// integers -- which is the shape the codec in `alac.cpp` always had. The module
// this replaced was that codec plus half an MP4 parser plus a file handle, and
// the half and the handle are `demux_mp4`'s now.
//
// Two things stay here because they are properties of ALAC rather than of MP4:
// the channel order, which is Apple's and not WAVE's, and the depth, which the
// magic cookie states and the container does not.

#include "alac.hpp"

#include <mediaperch/module.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    if (g_host == nullptr || g_host->log == nullptr) {
        return;
    }
    char buffer[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host->log(g_host->ctx, level, buffer);
}

std::uint32_t container_for(unsigned bits) noexcept
{
    if (bits <= 16) {
        return 2;
    }
    return bits <= 24 ? 3 : 4;
}

MpSampleType sample_type_for(std::uint32_t container, unsigned valid) noexcept
{
    switch (container) {
    case 2:
        return MP_SAMPLE_S16;
    case 3:
        return MP_SAMPLE_S24_PACKED;
    default:
        return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    }
}

} // namespace

struct MpCodecInstance {
    mp::alac::Decoder codec;
    MpFormat format{};
    std::uint32_t container = 0;
    unsigned shift = 0;
    std::vector<std::int32_t> decoded;
};

namespace {

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_ALAC) {
        return MP_OK;
    }
    // **A question about data, not about a file.** The cookie either parses or
    // it does not, and a codec that could not read this stream should say so
    // before the host commits to it rather than after.
    mp::alac::Config cfg;
    if (config != nullptr && mp::alac::parse_config(config, config_bytes, cfg)) {
        *out_score = 100;
    }
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const std::uint8_t* config,
                            std::uint32_t config_bytes, MpCodecInstance** out) noexcept
{
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    if (codec != MP_CODEC_ALAC) {
        return MP_ERR_UNSUPPORTED;
    }
    mp::alac::Config cfg;
    if (config == nullptr || !mp::alac::parse_config(config, config_bytes, cfg)) {
        log_fmt(MP_LOG_DEBUG,
                "the container's ALACSpecificConfig is %u bytes and does not parse",
                config_bytes);
        return MP_ERR_FORMAT;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    if (!c->codec.init(cfg)) {
        log_fmt(MP_LOG_WARN, "ALAC at %u Hz, %u channels, %u bits: the decoder refused it",
                cfg.sample_rate, static_cast<unsigned>(cfg.channels),
                static_cast<unsigned>(cfg.bit_depth));
        delete c;
        return MP_ERR_FORMAT;
    }

    // The depth is the cookie's, and the container it goes in is the smallest
    // one that holds it -- left-justified where that leaves room, which is what
    // the ABI means by `valid_bits`.
    c->container = container_for(c->codec.bit_depth());
    c->shift = c->container * 8u - c->codec.bit_depth();
    c->format.sample_rate = cfg.sample_rate;
    c->format.channels = c->codec.channels();
    c->format.channel_mask = mp::alac::layout_for(c->codec.channels()).mask;
    c->format.sample_type = sample_type_for(c->container, c->codec.bit_depth());
    c->format.encoding = MP_ENCODING_PCM;
    c->format.valid_bits = c->codec.bit_depth();
    c->decoded.resize(static_cast<std::size_t>(cfg.frame_length) * c->codec.channels());
    *out = c;
    return MP_OK;
}

MpResult MP_CALL codec_get_format(MpCodecInstance* c, MpFormat* out) noexcept
{
    if (c == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = c->format;
    return MP_OK;
}

MpResult MP_CALL codec_decode(MpCodecInstance* c, const void* packet,
                              std::size_t packet_bytes, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
{
    if (c == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;
    if (packet == nullptr || packet_bytes == 0) {
        return MP_ERR_INVALID;
    }

    const std::uint32_t frames =
        c->codec.decode(static_cast<const std::uint8_t*>(packet), packet_bytes,
                        c->decoded.data());
    if (frames == 0) {
        return MP_ERR_FORMAT;
    }

    const unsigned channels = c->codec.channels();
    const std::size_t needed =
        static_cast<std::size_t>(frames) * channels * c->container;
    if (dst == nullptr || dst_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }

    // **Apple's channel order into WAVE's**, which is the one thing about this
    // codec that a decoder can get perfectly right and still put every channel
    // in the wrong speaker. docs/formats.md records what happens when it is
    // skipped: eight of eight channels exact, none of them where they belong.
    const mp::alac::ChannelLayout& layout = mp::alac::layout_for(channels);
    auto* out = static_cast<std::uint8_t*>(dst);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const std::int32_t* in =
            c->decoded.data() + static_cast<std::size_t>(f) * channels;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const auto v = static_cast<std::uint32_t>(in[layout.from[ch]]) << c->shift;
            for (std::uint32_t b = 0; b < c->container; ++b) {
                *out++ = static_cast<std::uint8_t>((v >> (b * 8)) & 0xFFu);
            }
        }
    }
    *out_bytes = needed;
    return MP_OK;
}

MpResult MP_CALL codec_flush(MpCodecInstance* c, void* dst, std::size_t dst_bytes,
                             std::size_t* out_bytes) noexcept
{
    (void)c;
    (void)dst;
    (void)dst_bytes;
    if (out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    // ALAC is a packet in, a packet out. Nothing is held back, so there is
    // nothing to give back.
    *out_bytes = 0;
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // Every ALAC packet is decodable on its own -- there is no inter-packet
    // state to forget, which is why a seek in ALAC needs no pre-roll and a seek
    // in AAC does.
    return MP_OK;
}

void MP_CALL codec_close(MpCodecInstance* c) noexcept
{
    delete c;
}

const MpCodecVtbl g_vtbl = {
    /* size       */ sizeof(MpCodecVtbl),
    /* reserved   */ 0,
    /* probe      */ &codec_probe,
    /* open       */ &codec_open,
    /* get_format */ &codec_get_format,
    /* decode     */ &codec_decode,
    /* flush      */ &codec_flush,
    /* reset      */ &codec_reset,
    /* close      */ &codec_close,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpCodec g_codecs[] = {MP_CODEC_ALAC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 115,
    /* id          */ "codec_alac",
    /* name        */ "ALAC (written here, not vendored)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ 1,
    /* reserved    */ 0,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

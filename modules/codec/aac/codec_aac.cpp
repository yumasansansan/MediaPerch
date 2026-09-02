// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC-LC, as a codec and nothing else.
//
// Handed the `AudioSpecificConfig` the container found and then packets, one
// raw_data_block at a time, it produces float — which is what the codec makes
// and what every lossy decoder here reports, so a file it reads goes to Path B
// and the reason is written down rather than inferred.
//
// What is *not* here is what `decode_aac` also carried: half an MP4 parser, an
// ADTS framer, and a file handle. The first is `demux_mp4`'s; the second is a
// demuxer nobody has written yet, which is exactly the shape of the remaining
// work rather than a gap in this.

#include "aac.hpp"

#include <mediaperch/module.h>

#include <cstring>
#include <new>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

} // namespace

struct MpCodecInstance {
    mp::aac::Decoder codec;
    MpFormat format{};
    bool format_known = false;
    /// One frame, interleaved into WAVE order, waiting to be handed over.
    std::vector<float> ready;
};

namespace {

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_AAC_LC) {
        return MP_OK;
    }
    mp::aac::Config cfg;
    if (config != nullptr && mp::aac::parse_asc(config, config_bytes, cfg)) {
        // Object type 2 and nothing else. SBR and PS are a different codec
        // wearing the same name, and saying so here is what lets the host reach
        // `decode_ffmpeg` instead of finding out three packets in.
        *out_score = cfg.object_type == 2 ? 100u : 0u;
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
    if (codec != MP_CODEC_AAC_LC) {
        return MP_ERR_UNSUPPORTED;
    }
    mp::aac::Config cfg;
    if (config == nullptr || !mp::aac::parse_asc(config, config_bytes, cfg)) {
        return MP_ERR_FORMAT;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    if (!c->codec.init(cfg)) {
        delete c;
        return MP_ERR_FORMAT;
    }

    c->format.sample_rate = cfg.sample_rate;
    c->format.sample_type = MP_SAMPLE_F32;
    c->format.encoding = MP_ENCODING_PCM;
    // **The channel count may not be known yet.** Configuration 0 puts the
    // layout in a program config element, which an MP4's AudioSpecificConfig
    // carries and a raw ADTS stream does not -- so for that case the answer
    // arrives with the first frame, and `get_format` says so by failing until
    // then rather than by guessing stereo.
    if (cfg.channel_config != 0) {
        const auto& layout = mp::aac::layout_for_config(cfg.channel_config);
        c->format.channels = cfg.channel_config;
        c->format.channel_mask = layout.mask;
        c->format_known = true;
    } else if (cfg.pce.count != 0) {
        c->format.channels = cfg.pce.count;
        c->format.channel_mask = cfg.pce.mask;
        c->format_known = true;
    }
    *out = c;
    return MP_OK;
}

MpResult MP_CALL codec_get_format(MpCodecInstance* c, MpFormat* out) noexcept
{
    if (c == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    if (!c->format_known) {
        return MP_ERR_FORMAT;
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
    if (!c->codec.decode_frame(static_cast<const std::uint8_t*>(packet), packet_bytes)) {
        return MP_ERR_FORMAT;
    }

    const unsigned channels = c->codec.channels();
    if (channels == 0) {
        return MP_ERR_FORMAT;
    }
    if (!c->format_known) {
        // Configuration 0: the first frame carried the program config element,
        // so this is where the layout finally arrives.
        c->format.channels = channels;
        c->format.channel_mask = c->codec.layout().mask;
        c->format_known = true;
    }

    const std::size_t needed =
        static_cast<std::size_t>(mp::aac::k_frame_len) * channels * sizeof(float);
    if (dst == nullptr || dst_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }

    // Planar float into interleaved, in WAVE slot order. FFmpeg's encoder
    // writes configuration 0 for 7.1(wide) and puts the layout in a program
    // config element, so this table is not a corner nobody reaches -- getting
    // it wrong decodes every channel perfectly into the wrong speaker.
    const mp::aac::ChannelLayout& layout = c->codec.layout();
    auto* out = static_cast<float*>(dst);
    for (unsigned ch = 0; ch < channels; ++ch) {
        const float* in = c->codec.pcm(layout.from[ch]);
        for (unsigned n = 0; n < mp::aac::k_frame_len; ++n) {
            out[static_cast<std::size_t>(n) * channels + ch] = in[n];
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
    // The overlap of the last frame is the encoder's padding, and the
    // container's `play_frames` is what says how much of the audio was real.
    // Emitting it here would be emitting padding the edit exists to remove.
    *out_bytes = 0;
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // **AAC frames are not independent.** Each one is windowed against the last,
    // so a frame decoded with no predecessor is half a frame of silence
    // overlapped onto real audio. The host feeds the packet before the target
    // and discards it, which is what `MpCodecVtbl::reset` exists to make
    // possible -- and what v1 hid inside this decoder, where the host could not
    // see it and a second codec had to reimplement it.
    mp::aac::Config cfg;
    (void)cfg;
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

const MpCodec g_codecs[] = {MP_CODEC_AAC_LC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 108,
    /* id          */ "codec_aac",
    /* name        */ "AAC-LC (written here, not vendored)",
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

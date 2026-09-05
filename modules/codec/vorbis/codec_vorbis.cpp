// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vorbis, as a codec and nothing else.
//
// libvorbis is the reference decoder and `vorbisfile` is the convenience layer
// that reads Ogg for you. The module this replaced used the second and therefore
// could not read a Vorbis stream out of any other container. This uses the
// first, so the same codec works for Vorbis in Matroska or in WebM the day a
// demuxer for those exists.
//
// **Vorbis is the codec whose configuration does not fit in one blob.** It has
// three header packets -- identification, comment and setup -- and the setup
// header is the large one that describes the codebooks. Every container invents
// a framing for them: Matroska uses xiph lacing, MP4 uses an esds descriptor.
// This ABI writes its own down in `module.h` rather than leaving it to be
// discovered: each packet preceded by its length as a 32-bit little-endian
// integer.

#include <vorbis/codec.h>

#include <mediaperch/module.h>

#include <cstring>
#include <new>
#include <vector>

namespace {


/// Vorbis channel order into WAVE order, indexed by channel count.
///
/// The same table `codec_opus` uses, because Opus mapping family 1 is *defined*
/// as Vorbis order. Duplicated rather than shared because sharing it would mean
/// a library between two codec modules that otherwise have nothing to do with
/// each other -- and because the day one of them is wrong, having two copies is
/// how it gets noticed.
const std::uint8_t* wave_from_vorbis(unsigned channels) noexcept
{
    static const std::uint8_t k1[1] = {0};
    static const std::uint8_t k2[2] = {0, 1};
    static const std::uint8_t k3[3] = {0, 2, 1};
    static const std::uint8_t k4[4] = {0, 1, 2, 3};
    static const std::uint8_t k5[5] = {0, 2, 1, 3, 4};
    static const std::uint8_t k6[6] = {0, 2, 1, 5, 3, 4};
    static const std::uint8_t k7[7] = {0, 2, 1, 6, 5, 3, 4};
    static const std::uint8_t k8[8] = {0, 2, 1, 7, 5, 6, 3, 4};
    switch (channels) {
    case 1: return k1;
    case 2: return k2;
    case 3: return k3;
    case 4: return k4;
    case 5: return k5;
    case 6: return k6;
    case 7: return k7;
    case 8: return k8;
    default: return nullptr;
    }
}

std::uint32_t mask_for(unsigned channels) noexcept
{
    switch (channels) {
    case 1: return 0x4u;
    case 2: return 0x3u;
    case 3: return 0x7u;
    case 4: return 0x33u;
    case 5: return 0x37u;
    case 6: return 0x3Fu;
    case 7: return 0x70Fu;
    case 8: return 0x63Fu;
    default: return 0u;
    }
}

/// Splits the config blob into its three packets. The framing is this ABI's and
/// is written down in `module.h`; a blob that does not obey it is refused here
/// rather than fed to libvorbis to find out.
bool split_headers(const std::uint8_t* config, std::uint32_t bytes,
                   const std::uint8_t* out[3], std::uint32_t sizes[3]) noexcept
{
    if (config == nullptr) {
        return false;
    }
    std::uint32_t at = 0;
    for (int i = 0; i < 3; ++i) {
        if (bytes - at < 4) {
            return false;
        }
        const std::uint32_t n = static_cast<std::uint32_t>(config[at]) |
                                (static_cast<std::uint32_t>(config[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(config[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(config[at + 3]) << 24);
        at += 4;
        if (n == 0 || bytes - at < n) {
            return false;
        }
        out[i] = config + at;
        sizes[i] = n;
        at += n;
    }
    return true;
}

} // namespace

struct MpCodecInstance {
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    bool dsp_ready = false;
    bool block_ready = false;
    MpFormat format{};
    unsigned channels = 0;
    const std::uint8_t* order = nullptr;
};

namespace {

/// Feeds the three headers to libvorbis. Shared by `probe` and `open` so that
/// the two cannot disagree about what a readable stream is.
bool headers_in(vorbis_info& info, vorbis_comment& comment, const std::uint8_t* config,
                std::uint32_t bytes) noexcept
{
    const std::uint8_t* packets[3] = {};
    std::uint32_t sizes[3] = {};
    if (!split_headers(config, bytes, packets, sizes)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        ogg_packet packet{};
        packet.packet = const_cast<unsigned char*>(packets[i]);
        packet.bytes = static_cast<long>(sizes[i]);
        packet.b_o_s = i == 0 ? 1 : 0;
        packet.packetno = i;
        if (vorbis_synthesis_headerin(&info, &comment, &packet) < 0) {
            return false;
        }
    }
    return info.channels > 0 && info.rate > 0;
}

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_VORBIS) {
        return MP_OK;
    }
    vorbis_info info;
    vorbis_comment comment;
    vorbis_info_init(&info);
    vorbis_comment_init(&comment);
    const bool ok = headers_in(info, comment, config, config_bytes) &&
                    wave_from_vorbis(static_cast<unsigned>(info.channels)) != nullptr;
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    if (ok) {
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
    if (codec != MP_CODEC_VORBIS) {
        return MP_ERR_UNSUPPORTED;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    vorbis_info_init(&c->info);
    vorbis_comment_init(&c->comment);
    if (!headers_in(c->info, c->comment, config, config_bytes)) {
        vorbis_comment_clear(&c->comment);
        vorbis_info_clear(&c->info);
        delete c;
        return MP_ERR_FORMAT;
    }

    c->channels = static_cast<unsigned>(c->info.channels);
    c->order = wave_from_vorbis(c->channels);
    if (c->order == nullptr || vorbis_synthesis_init(&c->dsp, &c->info) != 0) {
        vorbis_comment_clear(&c->comment);
        vorbis_info_clear(&c->info);
        delete c;
        return MP_ERR_UNSUPPORTED;
    }
    c->dsp_ready = true;
    vorbis_block_init(&c->dsp, &c->block);
    c->block_ready = true;

    c->format.sample_rate = static_cast<std::uint32_t>(c->info.rate);
    c->format.channels = c->channels;
    c->format.channel_mask = mask_for(c->channels);
    // Float, because that is what libvorbis produces. Reaching an integer would
    // mean a conversion this program refuses to make without being asked --
    // which is why every file this reads goes to Path B, and why that is a
    // property of the codec rather than a decision of ours.
    c->format.sample_type = MP_SAMPLE_F32;
    c->format.encoding = MP_ENCODING_PCM;
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

/// Drains whatever libvorbis has finished into the caller's buffer.
MpResult drain(MpCodecInstance* c, void* dst, std::size_t dst_bytes,
               std::size_t* out_bytes) noexcept
{
    float** pcm = nullptr;
    const int frames = vorbis_synthesis_pcmout(&c->dsp, &pcm);
    if (frames <= 0) {
        *out_bytes = 0;
        return MP_OK;
    }
    const std::size_t needed =
        static_cast<std::size_t>(frames) * c->channels * sizeof(float);
    if (dst == nullptr || dst_bytes < needed) {
        // Nothing is consumed: `vorbis_synthesis_read` is what consumes, and it
        // has not been called. The host grows and asks again.
        *out_bytes = needed;
        return MP_ERR_NO_MEMORY;
    }

    // Planar into interleaved, in WAVE slot order.
    auto* out = static_cast<float*>(dst);
    for (unsigned ch = 0; ch < c->channels; ++ch) {
        const float* in = pcm[c->order[ch]];
        for (int f = 0; f < frames; ++f) {
            out[static_cast<std::size_t>(f) * c->channels + ch] = in[f];
        }
    }
    vorbis_synthesis_read(&c->dsp, frames);
    *out_bytes = needed;
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

    ogg_packet op{};
    op.packet = static_cast<unsigned char*>(const_cast<void*>(packet));
    op.bytes = static_cast<long>(packet_bytes);
    if (vorbis_synthesis(&c->block, &op) == 0) {
        vorbis_synthesis_blockin(&c->dsp, &c->block);
    }
    // A packet that produced nothing is not an error: the first one after a
    // reset is a window with no predecessor and yields no finished samples.
    return drain(c, dst, dst_bytes, out_bytes);
}

MpResult MP_CALL codec_flush(MpCodecInstance* c, void* dst, std::size_t dst_bytes,
                             std::size_t* out_bytes) noexcept
{
    if (c == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    // Whatever the last block finished and nobody has taken yet.
    return drain(c, dst, dst_bytes, out_bytes);
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr || !c->dsp_ready) {
        return MP_ERR_INVALID;
    }
    // Vorbis packets are windowed against their predecessor, so the audio after
    // a seek is not adjacent to the audio before it. `vorbis_synthesis_restart`
    // is libvorbis's own word for exactly this.
    return vorbis_synthesis_restart(&c->dsp) == 0 ? MP_OK : MP_ERR_INTERNAL;
}

void MP_CALL codec_close(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return;
    }
    if (c->block_ready) {
        vorbis_block_clear(&c->block);
    }
    if (c->dsp_ready) {
        vorbis_dsp_clear(&c->dsp);
    }
    vorbis_comment_clear(&c->comment);
    vorbis_info_clear(&c->info);
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
    (void)host; // nothing here logs, so nothing here keeps the host
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
}

const MpCodec g_codecs[] = {MP_CODEC_VORBIS};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 110,
    /* id          */ "codec_vorbis",
    /* name        */ "Vorbis (libvorbis, the reference decoder)",
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

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opus, as a codec and nothing else.
//
// The module this replaced reached Opus through `opusfile`, which is the
// container and the codec in one object -- convenient, and the reason an
// OggFLAC was "an Ogg that module cannot read". `opusfile` is not in the tree
// any more. This drives libopus itself, from the `OpusHead` the container
// handed over, and knows nothing about Ogg at all, so the same codec works for
// Opus in Matroska, in MP4, or in anything else that carries it, the day a
// demuxer for one of those exists.
//
// **Two things about Opus that are the codec's and not the container's.** It
// always decodes at 48 kHz, whatever rate the encoder was given -- that is the
// format, not a resample. And its channel mapping families put the channels in
// Vorbis order, which is not WAVE order, so the permutation happens here for
// the same reason ALAC's does.

#include <opus_multistream.h>

#include <mediaperch/module.h>

#include <cstring>
#include <new>
#include <vector>

namespace {


/// Vorbis channel order into WAVE order, indexed by channel count.
///
/// Opus mapping family 1 is defined as Vorbis order, which puts centre second
/// in a 5.1 file where WAVE puts it third. Getting this wrong decodes every
/// sample perfectly and puts the dialogue in a surround speaker -- the same
/// failure `docs/formats.md` records for multichannel ALAC in Media Foundation.
const std::uint8_t* wave_from_vorbis(unsigned channels) noexcept
{
    static const std::uint8_t k1[1] = {0};
    static const std::uint8_t k2[2] = {0, 1};
    static const std::uint8_t k3[3] = {0, 2, 1};             // L C R
    static const std::uint8_t k4[4] = {0, 1, 2, 3};          // L R Bl Br
    static const std::uint8_t k5[5] = {0, 2, 1, 3, 4};       // L C R Bl Br
    static const std::uint8_t k6[6] = {0, 2, 1, 5, 3, 4};    // L C R LFE Bl Br
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
    case 1: return 0x4u;                  // FC
    case 2: return 0x3u;                  // FL FR
    case 3: return 0x7u;                  // FL FR FC
    case 4: return 0x33u;                 // FL FR BL BR
    case 5: return 0x37u;                 // FL FR FC BL BR
    case 6: return 0x3Fu;                 // FL FR FC LFE BL BR
    case 7: return 0x70Fu;
    case 8: return 0x63Fu;
    default: return 0u;
    }
}

/// The largest frame Opus produces: 120 ms at 48 kHz.
constexpr int k_max_frame = 5760;

} // namespace

struct MpCodecInstance {
    OpusMSDecoder* decoder = nullptr;
    MpFormat format{};
    unsigned channels = 0;
    const std::uint8_t* order = nullptr;
    std::vector<float> planar;
};

namespace {

/// Reads what libopus needs out of an `OpusHead`, which is the whole of Opus's
/// configuration and is defined by RFC 7845 §5.1.
bool parse_head(const std::uint8_t* config, std::uint32_t bytes, int& channels,
                int& streams, int& coupled, unsigned char mapping[255]) noexcept
{
    if (config == nullptr || bytes < 19 || std::memcmp(config, "OpusHead", 8) != 0) {
        return false;
    }
    channels = config[9];
    if (channels < 1 || channels > 255) {
        return false;
    }
    const unsigned family = config[18];
    if (family == 0) {
        // Mono or stereo, one stream, and the mapping is the identity.
        if (channels > 2) {
            return false;
        }
        streams = 1;
        coupled = channels - 1;
        for (int i = 0; i < channels; ++i) {
            mapping[i] = static_cast<unsigned char>(i);
        }
        return true;
    }
    if (family != 1 && family != 255) {
        return false;
    }
    if (bytes < 21 + static_cast<std::uint32_t>(channels)) {
        return false;
    }
    streams = config[19];
    coupled = config[20];
    if (streams < 1 || coupled < 0 || coupled > streams) {
        return false;
    }
    std::memcpy(mapping, config + 21, static_cast<std::size_t>(channels));
    return true;
}

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_OPUS) {
        return MP_OK;
    }
    int channels = 0;
    int streams = 0;
    int coupled = 0;
    unsigned char mapping[255];
    if (parse_head(config, config_bytes, channels, streams, coupled, mapping)) {
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
    if (codec != MP_CODEC_OPUS) {
        return MP_ERR_UNSUPPORTED;
    }

    int channels = 0;
    int streams = 0;
    int coupled = 0;
    unsigned char mapping[255];
    if (!parse_head(config, config_bytes, channels, streams, coupled, mapping)) {
        return MP_ERR_FORMAT;
    }
    const std::uint8_t* order = wave_from_vorbis(static_cast<unsigned>(channels));
    if (order == nullptr) {
        return MP_ERR_UNSUPPORTED; // more than eight channels, which nothing here maps
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    int error = OPUS_OK;
    c->decoder = opus_multistream_decoder_create(48000, channels, streams, coupled,
                                                 mapping, &error);
    if (c->decoder == nullptr || error != OPUS_OK) {
        delete c;
        return MP_ERR_FORMAT;
    }

    c->channels = static_cast<unsigned>(channels);
    c->order = order;
    // **48 kHz, always.** The rate in the header is what the encoder was given;
    // what comes out is 48 kHz because that is what Opus is. Reporting the
    // input's rate would be reporting a resample that nobody performed.
    c->format.sample_rate = 48000;
    c->format.channels = c->channels;
    c->format.channel_mask = mask_for(c->channels);
    c->format.sample_type = MP_SAMPLE_F32;
    c->format.encoding = MP_ENCODING_PCM;
    c->planar.resize(static_cast<std::size_t>(k_max_frame) * c->channels);
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

    const int frames = opus_multistream_decode_float(
        c->decoder, static_cast<const unsigned char*>(packet),
        static_cast<opus_int32>(packet_bytes), c->planar.data(), k_max_frame, 0);
    if (frames < 0) {
        return MP_ERR_FORMAT;
    }
    if (frames == 0) {
        return MP_OK;
    }

    const std::size_t needed =
        static_cast<std::size_t>(frames) * c->channels * sizeof(float);
    if (dst == nullptr || dst_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }

    // libopus writes interleaved already, but in Vorbis channel order. The
    // permutation is in place over a copy rather than around the decode,
    // because the decoder's own buffer is the one it wrote.
    auto* out = static_cast<float*>(dst);
    for (int f = 0; f < frames; ++f) {
        const float* in = c->planar.data() + static_cast<std::size_t>(f) * c->channels;
        float* row = out + static_cast<std::size_t>(f) * c->channels;
        for (unsigned ch = 0; ch < c->channels; ++ch) {
            row[ch] = in[c->order[ch]];
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
    // Opus is a packet in, a packet out. The overlap that remains is the
    // encoder's padding, and the container's `play_frames` is what says how
    // much of the audio was real.
    *out_bytes = 0;
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr || c->decoder == nullptr) {
        return MP_ERR_INVALID;
    }
    // Opus packets are windowed against their predecessor, so a decoder started
    // mid-stream is half a frame wrong until it has one. Telling it to forget is
    // what makes the host's pre-roll after a seek mean something.
    return opus_multistream_decoder_ctl(c->decoder, OPUS_RESET_STATE) == OPUS_OK
               ? MP_OK
               : MP_ERR_INTERNAL;
}

void MP_CALL codec_close(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return;
    }
    if (c->decoder != nullptr) {
        opus_multistream_decoder_destroy(c->decoder);
    }
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

const MpCodec g_codecs[] = {MP_CODEC_OPUS};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 110,
    /* id          */ "codec_opus",
    /* name        */ "Opus (libopus, the reference decoder)",
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

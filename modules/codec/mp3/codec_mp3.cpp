// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPEG audio layers I, II and III, as a codec and nothing else.
//
// dr_mp3 has two APIs. `drmp3` opens a file, finds frames, reads the LAME tag
// and decodes, which is a pipeline and is what the module this replaced used.
// Underneath it is `drmp3dec_decode_frame`, which takes one frame's bytes and
// gives back its samples and knows nothing else -- exactly the shape
// MpCodecVtbl asks for. So this module is thin, and being thin is the evidence
// that the split fell where the seam already was.
//
// The output is float, and deliberately. MPEG's synthesis filterbank produces
// real numbers; dr_mp3 will happily hand back int16 instead, and that would be a
// quantisation performed inside a decoder, which nothing here does.
// DR_MP3_FLOAT_OUTPUT makes float what the decoder actually computes rather than
// something converted back up from int16.
//
// **What is configured, for a format that has no configuration.** MPEG audio
// carries no setup data at all -- every frame restates the rate and the channel
// mode in its own header. The container therefore hands over that first header,
// four bytes of it, which is enough for `get_format` to answer before anything
// has been decoded. Without it a host would have to decode a frame to learn what
// it was about to receive, and the ABI asks the question at open.

#include <mediaperch/module.h>

#include <dr_mp3.h>

#include <cstring>
#include <new>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

unsigned version_of(const std::uint8_t* h) noexcept { return (h[1] >> 3) & 0x3u; }
unsigned layer_of(const std::uint8_t* h) noexcept { return (h[1] >> 1) & 0x3u; }
unsigned mode_of(const std::uint8_t* h) noexcept { return (h[3] >> 6) & 0x3u; }

std::uint32_t rate_of(const std::uint8_t* h) noexcept
{
    static const std::uint32_t rates[4][4] = {{11025, 12000, 8000, 0},
                                              {0, 0, 0, 0},
                                              {22050, 24000, 16000, 0},
                                              {44100, 48000, 32000, 0}};
    return rates[version_of(h)][(h[2] >> 2) & 0x3u];
}

/// Which of the three layers this header describes, as an MpCodec.
MpCodec codec_of(const std::uint8_t* h) noexcept
{
    switch (layer_of(h)) {
    case 3: return MP_CODEC_MP1;
    case 2: return MP_CODEC_MP2;
    case 1: return MP_CODEC_MP3;
    default: return MP_CODEC_UNKNOWN;
    }
}

/// Whether a four-byte configuration blob is a frame header that describes
/// `codec`. Both halves matter: a blob that is not a header at all, and one that
/// is a header for a different layer than the container said.
bool describes(MpCodec codec, const std::uint8_t* config, std::uint32_t bytes) noexcept
{
    if (config == nullptr || bytes < 4) {
        return false;
    }
    if (config[0] != 0xFFu || (config[1] & 0xE0u) != 0xE0u) {
        return false;
    }
    if (version_of(config) == 1u || layer_of(config) == 0u) {
        return false;
    }
    return rate_of(config) != 0 && codec_of(config) == codec;
}

} // namespace

struct MpCodecInstance {
    drmp3dec dec{};
    MpFormat format{};
    std::uint32_t channels = 0;
};

namespace {

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_MP1 && codec != MP_CODEC_MP2 && codec != MP_CODEC_MP3) {
        return MP_OK;
    }
    if (describes(codec, config, config_bytes)) {
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
    if (!describes(codec, config, config_bytes)) {
        return MP_ERR_UNSUPPORTED;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    drmp3dec_init(&c->dec);
    c->channels = mode_of(config) == 3u ? 1u : 2u;
    c->format.sample_rate = rate_of(config);
    c->format.channels = c->channels;
    // MPEG audio states no channel layout: one channel is a channel and two are
    // a pair, and anything a container knows beyond that is the container's.
    c->format.channel_mask = 0;
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
    // dr_mp3 takes an int, and a frame that would not fit in one is not a frame.
    if (packet_bytes > 0x7FFFFFFFu) {
        return MP_ERR_INVALID;
    }
    // The largest frame any layer produces, in samples, which is what
    // drmp3dec_decode_frame may write whatever this packet turns out to hold.
    const std::size_t room =
        static_cast<std::size_t>(DRMP3_MAX_SAMPLES_PER_FRAME) * sizeof(float);
    if (dst == nullptr || dst_bytes < room) {
        return MP_ERR_NO_MEMORY;
    }

    drmp3dec_frame_info info{};
    const int frames = drmp3dec_decode_frame(
        &c->dec, static_cast<const drmp3_uint8*>(packet), static_cast<int>(packet_bytes),
        dst, &info);
    if (frames < 0) {
        return MP_ERR_FORMAT;
    }
    if (frames == 0) {
        // A frame that produced nothing: dr_mp3 does this for the first frame of
        // a stream whose bit reservoir it has not seen. Not an error, and not an
        // end -- the host asks for another.
        return MP_OK;
    }
    if (info.channels != 0 && static_cast<std::uint32_t>(info.channels) != c->channels) {
        // A channel mode that changed mid-stream. Legal in the format, absent
        // from every real file, and impossible to hand to a graph that has
        // already negotiated a layout -- so it is refused rather than silently
        // reinterpreted.
        return MP_ERR_FORMAT;
    }
    *out_bytes = static_cast<std::size_t>(frames) * c->channels * sizeof(float);
    return MP_OK;
}

MpResult MP_CALL codec_flush(MpCodecInstance* c, void* dst, std::size_t dst_bytes,
                             std::size_t* out_bytes) noexcept
{
    (void)dst;
    (void)dst_bytes;
    if (c == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    // A frame in, a frame out. What is left at the end is the encoder's padding,
    // and the container's `play_frames` is what says how much of the audio was
    // real.
    *out_bytes = 0;
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // The bit reservoir and the synthesis filterbank's overlap both survive a
    // frame boundary, so a decoder that carried them across a seek would be
    // wrong for the first frame after it. The demuxer's pre-roll is what makes
    // forgetting affordable.
    drmp3dec_init(&c->dec);
    return MP_OK;
}

void MP_CALL codec_close(MpCodecInstance* c) noexcept
{
    delete c;
}

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
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

const MpCodec g_codecs[] = {MP_CODEC_MP1, MP_CODEC_MP2, MP_CODEC_MP3};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 105,
    /* id          */ "codec_mp3",
    /* name        */ "MPEG audio layers I to III (dr_mp3)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ 3,
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

// SPDX-License-Identifier: GPL-3.0-or-later
//
// FLAC as a codec, on libFLAC: the reference decoder, driven one frame at a
// time.
//
// **libFLAC has no packet interface**, which is the interesting part of this
// file. Its decoder is a stream decoder: it pulls bytes through a read callback,
// finds its own frames and hands back samples. Turning that into "one packet in,
// its samples out" takes two things, and both are properties of the format
// rather than tricks:
//
//  * **The configuration is a stream prefix.** `fLaC`, a metadata-block header
//    and the STREAMINFO the container handed over are exactly the beginning of a
//    FLAC file, so the decoder is opened on those forty-two synthesised bytes
//    and learns the rate, the depth and the channel count from the same block it
//    would have read off disk.
//  * **Every FLAC frame stands alone**, so nothing is lost by clearing the
//    decoder's input between packets. `FLAC__stream_decoder_flush` does exactly
//    that -- it drops buffered input and goes back to looking for a frame sync,
//    without touching the metadata -- so each packet is decoded from a clean
//    start and a packet boundary can never be confused with a frame boundary.
//
// What this buys is not tidiness. `demux_ogg` has been reading OggFLAC and
// naming the codec since the Ogg split, with nothing to hand it to; the same
// applies to FLAC in MP4 and in Matroska the day those demuxers name it. One
// codec module, and every container that carries FLAC has a decoder.
//
// The output is the file's own samples in the file's own width, because for a
// lossless codec correct is an identity rather than a tolerance.

#include <mediaperch/module.h>

#include "pcm_format.hpp"

#include <FLAC/stream_decoder.h>

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
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host->log(g_host->ctx, level, buffer);
}

/// How many bytes a sample of `bits` occupies. Shared -- see
/// modules/shared/pcm_format, and the drift that put it there.
std::uint32_t container_for(std::uint32_t bits) noexcept
{
    return mp::pcm::container_for(bits);
}

/// The sample type for `valid` significant bits in a `container`-byte slot.
///
/// **Its own copy of this used to live here**, as it did in five other modules,
/// and the copies had drifted -- see modules/shared/pcm_format.
MpSampleType sample_type_for(std::uint32_t container, std::uint32_t valid) noexcept
{
    return mp::pcm::sample_type_for(container, valid);
}

/// Reads the four fields of a STREAMINFO this module needs. The block is 34
/// bytes and its layout is fixed, which is why the ABI can define it as the
/// configuration blob and mean something precise.
bool read_streaminfo(const std::uint8_t* p, std::uint32_t bytes, std::uint32_t& rate,
                     std::uint32_t& channels, std::uint32_t& bits) noexcept
{
    if (p == nullptr || bytes < 34) {
        return false;
    }
    const std::uint32_t packed = (static_cast<std::uint32_t>(p[10]) << 24) |
                                 (static_cast<std::uint32_t>(p[11]) << 16) |
                                 (static_cast<std::uint32_t>(p[12]) << 8) |
                                 static_cast<std::uint32_t>(p[13]);
    rate = packed >> 12;
    channels = ((packed >> 9) & 0x7u) + 1;
    bits = ((packed >> 4) & 0x1Fu) + 1;
    return rate != 0 && channels >= 1 && channels <= 8 && bits >= 4 && bits <= 32;
}

} // namespace

struct MpCodecInstance {
    FLAC__StreamDecoder* decoder = nullptr;

    MpFormat format{};
    std::uint32_t bits = 0;
    std::uint32_t container = 0;
    std::uint32_t frame_bytes = 0;
    /// How far left a sample moves to sit at the top of its container. Zero for
    /// 16, 24 and 32 bits; FLAC also allows 4, 8, 12 and 20.
    std::uint32_t shift = 0;

    /// What the read callback is serving: the synthesised header at open, then
    /// one packet at a time.
    const std::uint8_t* input = nullptr;
    std::size_t input_bytes = 0;
    std::size_t input_at = 0;

    /// Where the write callback puts what it decoded.
    std::uint8_t* out = nullptr;
    std::size_t out_room = 0;
    std::size_t out_bytes = 0;
    bool overflowed = false;
    bool wrote = false;
    bool failed = false;

    ~MpCodecInstance()
    {
        if (decoder != nullptr) {
            FLAC__stream_decoder_delete(decoder);
        }
    }
};

namespace {

FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder*, FLAC__byte buffer[],
                                            std::size_t* bytes, void* client)
{
    auto* c = static_cast<MpCodecInstance*>(client);
    const std::size_t left = c->input_bytes - c->input_at;
    if (left == 0) {
        // **The end of the packet, not the end of the stream** -- but from
        // inside libFLAC those look the same, and this is the only status that
        // means "no more bytes". `flush` before the next packet is what puts the
        // decoder back to work, and it is why this is safe to say.
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    const std::size_t take = *bytes < left ? *bytes : left;
    std::memcpy(buffer, c->input + c->input_at, take);
    c->input_at += take;
    *bytes = take;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder*,
                                              const FLAC__Frame* frame,
                                              const FLAC__int32* const buffer[],
                                              void* client)
{
    auto* c = static_cast<MpCodecInstance*>(client);
    if (c->frame_bytes == 0) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    const std::uint32_t frames = frame->header.blocksize;
    const std::uint32_t channels = c->format.channels;
    const std::size_t needed = static_cast<std::size_t>(frames) * c->frame_bytes;
    if (c->out == nullptr || c->out_bytes + needed > c->out_room) {
        c->overflowed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // libFLAC hands over the sample's own value, sign-extended into an int32 and
    // right-aligned. Our containers are left-justified, which for 16, 24 and 32
    // bits is the same thing and for 20 or 12 is not.
    std::uint8_t* out = c->out + c->out_bytes;
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto value = static_cast<std::uint32_t>(buffer[ch][i]) << c->shift;
            for (std::uint32_t b = 0; b < c->container; ++b) {
                *out++ = static_cast<std::uint8_t>((value >> (b * 8)) & 0xFFu);
            }
        }
    }
    c->out_bytes += needed;
    c->wrote = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void error_callback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status,
                    void* client)
{
    auto* c = static_cast<MpCodecInstance*>(client);
    c->failed = true;
    log_fmt(MP_LOG_WARN, "libFLAC: %s", FLAC__StreamDecoderErrorStatusString[status]);
}

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (codec != MP_CODEC_FLAC) {
        return MP_OK;
    }
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    if (read_streaminfo(config, config_bytes, rate, channels, bits) &&
        container_for(bits) != 0) {
        // 100: for a lossless codec the reference implementation *is* the
        // specification, so a second FLAC codec should have to argue its way
        // past this one rather than tie with it. [formats.md](../../../docs/formats.md)
        // records what a reimplementation costs -- dr_flac decodes a 32-bit FLAC
        // to nothing at all, silently, because its frame-header table still
        // marks the bit-depth code FLAC 1.4 assigned to 32 bits as reserved.
        *out_score = 100;
    }
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const std::uint8_t* config,
                            std::uint32_t config_bytes, MpCodecInstance** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    if (codec != MP_CODEC_FLAC) {
        return MP_ERR_UNSUPPORTED;
    }
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    if (!read_streaminfo(config, config_bytes, rate, channels, bits)) {
        return MP_ERR_FORMAT;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->bits = bits;
    c->container = container_for(bits);
    c->shift = c->container != 0 ? c->container * 8 - bits : 0;
    c->format.sample_rate = rate;
    c->format.channels = channels;
    c->format.channel_mask = 0; // FLAC's channel assignment is implicit
    c->format.encoding = MP_ENCODING_PCM;
    c->format.valid_bits = bits;
    c->format.sample_type = sample_type_for(c->container, bits);
    c->frame_bytes = c->container * channels;
    if (c->container == 0 || c->format.sample_type == MP_SAMPLE_NONE) {
        delete c;
        return MP_ERR_UNSUPPORTED;
    }

    c->decoder = FLAC__stream_decoder_new();
    if (c->decoder == nullptr) {
        delete c;
        return MP_ERR_NO_MEMORY;
    }
    // **The MD5 check is off, and that is a real loss worth naming.** A FLAC
    // file carries an MD5 of its own unencoded audio, and libFLAC will check
    // what it produced against it -- a bit-exactness proof from inside the
    // file, and the only one in this tree that does not depend on a second
    // decoder agreeing. It cannot survive the split: the sum is over the whole
    // stream in order, and a codec sees the packets a container chose to give
    // it, in whatever order playback asked for. Verifying it would have to
    // become the host's, over a decode that ran start to finish, which is a
    // thing `mediaperch-probe decode` could do and does not yet.
    FLAC__stream_decoder_set_md5_checking(c->decoder, false);

    if (FLAC__stream_decoder_init_stream(c->decoder, &read_callback, nullptr, nullptr,
                                         nullptr, nullptr, &write_callback, nullptr,
                                         &error_callback,
                                         c) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        delete c;
        return MP_ERR_INTERNAL;
    }

    // The beginning of a FLAC file, assembled from the blob: the magic, a
    // metadata-block header saying "STREAMINFO, last block, 34 bytes", and the
    // block. The decoder reads it as it would read a file and comes out
    // configured.
    std::vector<std::uint8_t> prefix;
    prefix.reserve(42);
    prefix.insert(prefix.end(), {'f', 'L', 'a', 'C'});
    prefix.insert(prefix.end(), {0x80, 0x00, 0x00, 0x22});
    prefix.insert(prefix.end(), config, config + 34);
    c->input = prefix.data();
    c->input_bytes = prefix.size();
    c->input_at = 0;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(c->decoder) || c->failed) {
        delete c;
        return MP_ERR_FORMAT;
    }
    c->input = nullptr;
    c->input_bytes = 0;
    c->input_at = 0;

    *out = c;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
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

    c->input = static_cast<const std::uint8_t*>(packet);
    c->input_bytes = packet_bytes;
    c->input_at = 0;
    c->out = static_cast<std::uint8_t*>(dst);
    c->out_room = dst_bytes;
    c->out_bytes = 0;
    c->overflowed = false;
    c->wrote = false;
    c->failed = false;

    // Back to looking for a frame sync, with nothing buffered from the packet
    // before. The metadata survives this, which is the whole reason the packet
    // interface can be built out of a stream one.
    if (!FLAC__stream_decoder_flush(c->decoder)) {
        return MP_ERR_INTERNAL;
    }
    const bool ok = FLAC__stream_decoder_process_single(c->decoder) != 0;
    c->input = nullptr;
    c->input_bytes = 0;

    if (c->overflowed) {
        return MP_ERR_NO_MEMORY;
    }
    if (!ok || c->failed) {
        return MP_ERR_FORMAT;
    }
    *out_bytes = c->out_bytes;
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
    *out_bytes = 0; // a frame in, a frame out: nothing is ever held back
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr || c->decoder == nullptr) {
        return MP_ERR_INVALID;
    }
    // Nothing carries between FLAC frames, so this is already true after every
    // packet. It is honoured anyway rather than returning "unsupported", because
    // a host is entitled to be told that forgetting worked.
    return FLAC__stream_decoder_flush(c->decoder) != 0 ? MP_OK : MP_ERR_INTERNAL;
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

const MpCodec g_codecs[] = {MP_CODEC_FLAC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 120,
    /* id          */ "codec_flac",
    /* name        */ "FLAC (libFLAC, the reference decoder)",
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

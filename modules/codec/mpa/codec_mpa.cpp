// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPEG audio -- layers I, II and III -- as a codec and nothing else, on
// libmpg123.
//
// **The name is `mpa`, not `mp3`, because the module was never only MP3.** It
// has always decoded all three layers and claimed `MP_CODEC_MP1`, `MP_CODEC_MP2`
// and `MP_CODEC_MP3`; the name said one of the three. MPA is what RFC 3551 calls
// the family and what FFmpeg and GStreamer call it internally.
//
// It never sees a file. `demux_mpa` hands over the first frame's header as the
// configuration blob -- MPEG audio has no setup data, so its own header is the
// only thing there is to state -- and then packets, one frame at a time.
//
// **Fed frame by frame, decoded frame by frame.** mpg123's usual mode owns the
// file and finds its own frames; that is the demuxer's job here, so this uses
// the feed interface instead: `mpg123_open_feed`, then `mpg123_feed` with one
// packet and `mpg123_decode_frame` to take the samples back out. No I/O, no
// seeking, no tag parsing -- all of that belongs to the half that has the file.
//
// **Gapless is off, deliberately.** mpg123 can apply the LAME tag's delay and
// padding itself, and if it did, the edit would be applied twice: `demux_mpa`
// states it in `MpStreamInfo` and `PacketSource` applies it, which is what §4
// means by the edit being the container's.

#include <mediaperch/module.h>

#include "module_log.hpp"

#include <mpg123.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    // The body is modules/shared/module_log: twelve copies of it had drifted to
    // three buffer sizes. Only the wrapper stays, because a `...` function
    // cannot forward to another one -- the va_list has to be made here.
    va_list args;
    va_start(args, format);
    mp::log::vfmt(g_host, level, format, args);
    va_end(args);
}

/// The header fields this module reads for itself, to answer `probe` and to
/// report a format before a single frame has been decoded.
unsigned version_of(const std::uint8_t* h) noexcept { return (h[1] >> 3) & 0x3u; }
unsigned layer_of(const std::uint8_t* h) noexcept { return (h[1] >> 1) & 0x3u; }
unsigned mode_of(const std::uint8_t* h) noexcept { return (h[3] >> 6) & 0x3u; }

/// Which of the three layers this header describes, as an MpCodec. The
/// bitstream numbers them backwards: 3 is layer I, 1 is layer III.
MpCodec codec_of(const std::uint8_t* h) noexcept
{
    switch (layer_of(h)) {
    case 3:
        return MP_CODEC_MP1;
    case 2:
        return MP_CODEC_MP2;
    case 1:
        return MP_CODEC_MP3;
    default:
        return MP_CODEC_UNKNOWN;
    }
}

std::uint32_t rate_of(const std::uint8_t* h) noexcept
{
    static const std::uint32_t table[4] = {44100, 48000, 32000, 0};
    const std::uint32_t base = table[(h[2] >> 2) & 0x3u];
    switch (version_of(h)) {
    case 3:
        return base; // MPEG 1
    case 2:
        return base / 2; // MPEG 2
    case 0:
        return base / 4; // MPEG 2.5
    default:
        return 0; // reserved
    }
}

/// Whether a four-byte configuration blob describes a frame this decodes.
bool plausible(const std::uint8_t* config, std::uint32_t bytes) noexcept
{
    if (config == nullptr || bytes < 4) {
        return false;
    }
    if (config[0] != 0xFFu || (config[1] & 0xE0u) != 0xE0u) {
        return false;
    }
    if (version_of(config) == 1u || layer_of(config) == 0u) {
        return false; // reserved version, reserved layer
    }
    return rate_of(config) != 0;
}

/// The largest number of samples any layer produces from one frame, which is
/// what a decode may write however long this packet turns out to be. Layer II
/// and layer III at MPEG-1 are 1152; the buffer is generous rather than tight.
constexpr std::size_t k_max_samples_per_frame = 1152;

} // namespace

struct MpCodecInstance {
    mpg123_handle* mh = nullptr;
    MpFormat format{};
    bool format_known = false;
    /// One frame of interleaved float, copied out of mpg123's own buffer.
    /// Bytes rather than floats: what mpg123 hands back is a byte count.
    std::vector<std::uint8_t> pcm;
};

namespace {

/// A fresh handle, configured the one way this module uses it: fed by hand,
/// float out, no gapless, quiet.
mpg123_handle* make_handle(std::uint32_t rate, std::uint32_t channels) noexcept
{
    int err = MPG123_OK;
    mpg123_handle* mh = mpg123_new(nullptr, &err);
    if (mh == nullptr) {
        return nullptr;
    }
    // ID3v2 skipped for the reason demux_mpa gives: mpg123 allocates a tag's
    // declared length before reading it, and this module reads no tags. A codec
    // is fed one frame at a time and should never see a tag at all, which makes
    // this belt as well as braces.
    mpg123_param2(mh, MPG123_ADD_FLAGS,
                  MPG123_QUIET | MPG123_FORCE_FLOAT | MPG123_SKIP_ID3V2, 0.0);
    mpg123_param2(mh, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0.0);

    // **One output format and no resampling.** libmpg123 will happily convert
    // rates and duplicate channels if asked; a decoder here never converts, so
    // it is allowed exactly the format the stream is in. If a file changes rate
    // mid-stream, decoding stops rather than silently resampling.
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, static_cast<long>(rate), static_cast<int>(channels),
                      MPG123_ENC_FLOAT_32) != MPG123_OK ||
        mpg123_open_feed(mh) != MPG123_OK) {
        mpg123_delete(mh);
        return nullptr;
    }
    return mh;
}

/// Takes everything libmpg123 currently has, into `c->pcm`, and says how many
/// bytes that was.
///
/// **The feed interface is pipelined**, which is the whole reason `flush` below
/// does any work: `mpg123_feed` with frame N and then one decode does not
/// necessarily give frame N's samples, and after the last packet there is a
/// frame still inside. Measured before this existed: 77 frames out of a
/// 78-frame file, silently, which is one frame of audio missing off the end.
MpResult drain(MpCodecInstance* c, std::size_t& produced) noexcept
{
    produced = 0;
    for (;;) {
        std::int64_t number = 0;
        unsigned char* audio = nullptr;
        std::size_t got = 0;
        const int status = mpg123_decode_frame64(c->mh, &number, &audio, &got);
        if (status == MPG123_NEW_FORMAT) {
            // **A message, not a failure, and it always arrives on the first
            // frame**: mpg123 is saying the output format is now known. What
            // would be a failure is that format being different from the one
            // this instance was opened on -- a stream that changes rate or
            // channel count mid-file -- because a decoder here never converts.
            // So it is checked rather than assumed either way.
            long rate = 0;
            int channels = 0;
            int encoding = 0;
            if (mpg123_getformat(c->mh, &rate, &channels, &encoding) == MPG123_OK &&
                (static_cast<std::uint32_t>(rate) != c->format.sample_rate ||
                 static_cast<std::uint32_t>(channels) != c->format.channels)) {
                log_fmt(MP_LOG_WARN,
                        "the stream became %ld Hz / %d ch mid-file, from %u / %u; "
                        "not converting",
                        rate, channels, c->format.sample_rate, c->format.channels);
                return MP_ERR_FORMAT;
            }
            continue;
        }
        if (status == MPG123_NEED_MORE || status == MPG123_DONE) {
            return MP_OK; // everything it had is out
        }
        if (status != MPG123_OK) {
            return MP_ERR_FORMAT;
        }
        if (got == 0 || audio == nullptr) {
            continue;
        }
        if (produced + got > c->pcm.size()) {
            c->pcm.resize(produced + got);
        }
        std::memcpy(c->pcm.data() + produced, audio, got);
        produced += got;
    }
}

/// Hands `produced` bytes of `c->pcm` to a caller, or says the buffer is small.
MpResult deliver(MpCodecInstance* c, std::size_t produced, void* dst,
                 std::size_t dst_bytes, std::size_t* out_bytes) noexcept
{
    // **A frame that produced nothing is not an error.** The first frame of a
    // layer III stream often decodes to nothing while the bit reservoir fills,
    // and the ABI says so: MP_OK with zero bytes.
    if (produced == 0) {
        return MP_OK;
    }
    if (dst == nullptr || dst_bytes < produced) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(dst, c->pcm.data(), produced);
    *out_bytes = produced;
    return MP_OK;
}

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
    // **A question about data.** The blob is a frame header or it is not, and a
    // header for a different layer than the container named is a disagreement
    // worth declining on rather than discovering later.
    if (plausible(config, config_bytes) && codec_of(config) == codec) {
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
    if (codec != MP_CODEC_MP1 && codec != MP_CODEC_MP2 && codec != MP_CODEC_MP3) {
        return MP_ERR_UNSUPPORTED;
    }
    if (!plausible(config, config_bytes)) {
        log_fmt(MP_LOG_DEBUG, "the container's %u-byte MPEG audio header does not parse",
                config_bytes);
        return MP_ERR_FORMAT;
    }

    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }

    // What the header says, before anything is decoded: `get_format` is asked
    // at open and the answer has to be right then.
    c->format.sample_rate = rate_of(config);
    c->format.channels = mode_of(config) == 3u ? 1u : 2u;
    c->format.channel_mask = 0;
    c->format.sample_type = MP_SAMPLE_F32;
    c->format.encoding = MP_ENCODING_PCM;
    c->format.valid_bits = 32;

    c->mh = make_handle(c->format.sample_rate, c->format.channels);
    if (c->mh == nullptr) {
        log_fmt(MP_LOG_WARN, "MPEG audio at %u Hz, %u channels: libmpg123 refused it",
                c->format.sample_rate, c->format.channels);
        delete c;
        return MP_ERR_FORMAT;
    }
    c->pcm.resize(k_max_samples_per_frame * c->format.channels * sizeof(float));
    c->format_known = true;
    *out = c;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_get_format(MpCodecInstance* c, MpFormat* out) noexcept
{
    if (c == nullptr || out == nullptr || !c->format_known) {
        return MP_ERR_INVALID;
    }
    *out = c->format;
    return MP_OK;
}

MpResult MP_CALL codec_decode(MpCodecInstance* c, const void* packet,
                              std::size_t packet_bytes, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
try {
    if (c == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;
    if (packet == nullptr || packet_bytes == 0) {
        return MP_ERR_INVALID;
    }

    if (mpg123_feed(c->mh, static_cast<const unsigned char*>(packet), packet_bytes) !=
        MPG123_OK) {
        return MP_ERR_FORMAT;
    }

    std::size_t produced = 0;
    const MpResult r = drain(c, produced);
    if (r != MP_OK) {
        return r;
    }
    return deliver(c, produced, dst, dst_bytes, out_bytes);
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_flush(MpCodecInstance* c, void* dst, std::size_t dst_bytes,
                             std::size_t* out_bytes) noexcept
try {
    if (c == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;
    // **What is still inside after the last packet, and there is some.** The
    // decoder that preceded this one was frame in, frame out and held nothing;
    // libmpg123's feed interface is pipelined and keeps a frame. Returning 0
    // here loses the last 1152 samples of every file, which is 26 milliseconds
    // that nothing else in the tree would have noticed.
    std::size_t produced = 0;
    const MpResult r = drain(c, produced);
    if (r != MP_OK) {
        return r;
    }
    return deliver(c, produced, dst, dst_bytes, out_bytes);
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // **The bit reservoir is exactly what this forgets.** After a seek the
    // audio is not adjacent to what came before, so a layer III decoder holding
    // the previous frames' bytes would reconstruct the first frame from the
    // wrong ones. The host feeds pre-roll frames afterwards and discards them.
    if (mpg123_open_feed(c->mh) != MPG123_OK) {
        return MP_ERR_INTERNAL;
    }
    return MP_OK;
}

void MP_CALL codec_close(MpCodecInstance* c) noexcept
{
    if (c == nullptr) {
        return;
    }
    if (c->mh != nullptr) {
        mpg123_close(c->mh);
        mpg123_delete(c->mh);
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
    g_host = host;
    return mpg123_init() == MPG123_OK ? MP_OK : MP_ERR_INTERNAL;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpCodec g_codecs[] = {MP_CODEC_MP1, MP_CODEC_MP2, MP_CODEC_MP3};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 2, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 105,
    /* id          */ "codec_mpa",
    /* name        */ "MPEG audio, layers I to III (libmpg123)",
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

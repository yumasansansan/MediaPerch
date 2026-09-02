// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCM: the codec that is a memcpy.
//
// **This module is short on purpose and is not a joke.** Uncompressed audio has
// a codec in exactly the sense every other stream does -- a container says
// MP_CODEC_PCM and something has to turn its packets into the samples the graph
// reads -- and under v1 that something was hidden inside whichever decoder
// happened to read the container. Writing it down has three consequences worth
// the file:
//
//  * **Every container that carries PCM gets a decoder for free.** WAV, AIFF and
//    W64 today; MP4 (`sowt`, `twos`, `lpcm`), Matroska and CAF the day their
//    demuxers name the codec. Under v1 each of those would have needed its own
//    copy of "and if it is uncompressed, hand the bytes over".
//  * **It has no dependency of any kind** -- not dr_libs, not the C++ standard
//    library beyond <cstring>. So the tree's oldest promise, that an install
//    with nothing else on disk still plays music, now rests on two small modules
//    rather than on one large one.
//  * **It cannot convert, because there is nothing here that could.** Path A's
//    bit-exactness used to be a property of `decode_native` behaving itself.
//    Here it is a property of the code: a memcpy has no other behaviour.
//
// The format is the container's. PCM's configuration blob is empty -- there is
// nothing a codec could be configured *with* -- so `get_format` declines to
// answer and `MpStreamInfo::format` stands, which is what the ABI says of
// MP_CODEC_PCM in as many words.

#include <mediaperch/module.h>

#include <cstring>
#include <new>

namespace {

const MpHost* g_host = nullptr;

} // namespace

struct MpCodecInstance {
    // Nothing. There is no state in copying bytes, which is also why `reset`
    // after a seek has nothing to forget.
    char unused = 0;
};

namespace {

MpResult MP_CALL codec_probe(MpCodec codec, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    (void)config;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    // A blob would mean the container thinks PCM needs configuring, and this
    // module would be the wrong one for whatever it thinks that is.
    *out_score = (codec == MP_CODEC_PCM && config_bytes == 0) ? 100u : 0u;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const std::uint8_t* config,
                            std::uint32_t config_bytes, MpCodecInstance** out) noexcept
{
    (void)config;
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;
    if (codec != MP_CODEC_PCM || config_bytes != 0) {
        return MP_ERR_UNSUPPORTED;
    }
    auto* c = new (std::nothrow) MpCodecInstance();
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    *out = c;
    return MP_OK;
}

MpResult MP_CALL codec_get_format(MpCodecInstance* c, MpFormat* out) noexcept
{
    (void)out;
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // **Deliberately no answer.** The rate, the width and the channel layout are
    // the container's statement about the file, and this module was handed none
    // of them -- PCM's configuration blob is empty because MpStreamInfo::format
    // is the whole of it. Reporting a format assembled from nothing would be
    // this module contradicting the only party that knows.
    return MP_ERR_UNSUPPORTED;
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
    if (dst == nullptr || dst_bytes < packet_bytes) {
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(dst, packet, packet_bytes);
    *out_bytes = packet_bytes;
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
    *out_bytes = 0; // nothing is ever held back
    return MP_OK;
}

MpResult MP_CALL codec_reset(MpCodecInstance* c) noexcept
{
    return c == nullptr ? MP_ERR_INVALID : MP_OK;
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

const MpCodec g_codecs[] = {MP_CODEC_PCM};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_CODEC,
    /* priority    */ 100,
    /* id          */ "codec_pcm",
    /* name        */ "PCM (a memcpy, and no dependencies at all)",
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

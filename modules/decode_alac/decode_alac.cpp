// SPDX-License-Identifier: GPL-3.0-or-later
//
// ALAC, decoded by this project rather than by anything it links to.
//
// The argument for writing a decoder instead of vendoring one is in §7 of
// docs/plan.md and in alac.hpp. The short version: ALAC's reference
// implementation is simultaneously the specification and abandoned since 2011,
// and ALHACK is what happened to the two chipset vendors who shipped it. Reading
// it to learn the format is right; linking it is not.
//
// This module has no dependencies at all -- not a submodule, not a runtime
// library, not an OS codec. It is the only decoder here that is entirely ours,
// which is a claim about *responsibility* rather than about quality: when it is
// wrong, there is nobody else to wait for.

#include "alac.hpp"
#include "mp4.hpp"

#include <mediaperch/module.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log(MpLogLevel level, const char* message) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, message);
    }
}

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log(level, buffer);
}

FILE* open_utf8(const char* path) noexcept
{
#if defined(_WIN32)
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wide_len <= 0) {
        return nullptr;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_len));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), wide_len);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, wide.data(), L"rb") != 0) {
        return nullptr;
    }
    return fp;
#else
    return std::fopen(path, "rb");
#endif
}

/// The container for `bits`, in bytes, following the same rule as decode_flac.
std::uint32_t container_for(unsigned bits) noexcept
{
    if (bits <= 16) {
        return 2;
    }
    if (bits <= 24) {
        return 3;
    }
    return 4;
}

MpSampleType sample_type_for(std::uint32_t container, unsigned valid) noexcept
{
    if (container == 2 && valid <= 16) {
        return MP_SAMPLE_S16;
    }
    if (container == 3) {
        return MP_SAMPLE_S24_PACKED;
    }
    if (container == 4) {
        return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    }
    return MP_SAMPLE_NONE;
}

/// `moov` is a top-level box, and where it sits is the muxer's choice: FFmpeg
/// writes it after the audio, refalac writes it before. Walking the top level by
/// header alone finds it either way, and reads only the box it wants.
bool read_moov(FILE* fp, std::vector<std::uint8_t>& out) noexcept
{
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        return false;
    }
    const std::int64_t file_bytes = _ftelli64(fp);
    if (file_bytes < 8) {
        return false;
    }

    std::int64_t at = 0;
    // A file is a short list of top-level boxes. A thousand is already absurd
    // and bounds the walk against a file that describes a loop of empty ones.
    for (int guard = 0; guard < 1000 && at + 8 <= file_bytes; ++guard) {
        if (_fseeki64(fp, at, SEEK_SET) != 0) {
            return false;
        }
        std::uint8_t header[16];
        if (std::fread(header, 1, 8, fp) != 8) {
            return false;
        }
        std::uint64_t size = (static_cast<std::uint32_t>(header[0]) << 24) |
                             (static_cast<std::uint32_t>(header[1]) << 16) |
                             (static_cast<std::uint32_t>(header[2]) << 8) |
                             static_cast<std::uint32_t>(header[3]);
        std::int64_t header_bytes = 8;
        if (size == 1) {
            if (std::fread(header + 8, 1, 8, fp) != 8) {
                return false;
            }
            size = 0;
            for (int i = 0; i < 8; ++i) {
                size = (size << 8) | header[8 + i];
            }
            header_bytes = 16;
        } else if (size == 0) {
            size = static_cast<std::uint64_t>(file_bytes - at);
        }
        if (size < static_cast<std::uint64_t>(header_bytes) ||
            at + static_cast<std::int64_t>(size) > file_bytes) {
            return false;
        }

        const bool is_moov = std::memcmp(header + 4, "moov", 4) == 0;
        if (is_moov) {
            const std::uint64_t body = size - static_cast<std::uint64_t>(header_bytes);
            // A moov large enough to matter is a moov describing millions of
            // packets, and mp4.cpp bounds those separately. 256 MB is far past
            // any real file and still a bound.
            if (body == 0 || body > (256ull << 20)) {
                return false;
            }
            out.resize(static_cast<std::size_t>(body));
            if (_fseeki64(fp, at + header_bytes, SEEK_SET) != 0) {
                return false;
            }
            return std::fread(out.data(), 1, out.size(), fp) == out.size();
        }
        at += static_cast<std::int64_t>(size);
    }
    return false;
}

} // namespace

struct MpDecoder {
    FILE* fp = nullptr;
    mp::mp4::AudioTrack track;
    mp::alac::Decoder codec;
    MpFormat format{};
    std::uint32_t container = 0;
    unsigned shift = 0; ///< left-justification, container*8 - bit_depth

    std::vector<std::uint8_t> packet;   ///< the compressed bytes of one packet
    std::vector<std::int32_t> decoded;  ///< interleaved, ALAC channel order
    std::vector<std::uint8_t> ready;    ///< packed output, WAVE order
    std::size_t ready_at = 0;           ///< bytes of `ready` already handed out

    std::size_t next_packet = 0;
    std::uint64_t frames_out = 0; ///< frames handed to the caller since seek
    std::uint64_t skip = 0;       ///< frames to drop from the next decode
    std::string path;
};

namespace {

/// Packs one decoded packet into `d->ready`: ALAC channel order to WAVE order,
/// right-justified samples to the project's left-justified containers, native
/// int32 to little-endian bytes. A permutation and a shift, and nothing else --
/// no value is combined with any other.
void pack(MpDecoder* d, std::uint32_t frames) noexcept
{
    const unsigned channels = d->codec.channels();
    const mp::alac::ChannelLayout& layout = mp::alac::layout_for(channels);
    const std::uint32_t container = d->container;
    const unsigned shift = d->shift;

    d->ready.resize(static_cast<std::size_t>(frames) * channels * container);
    d->ready_at = 0;

    std::uint8_t* out = d->ready.data();
    for (std::uint32_t f = 0; f < frames; ++f) {
        const std::int32_t* in = d->decoded.data() + static_cast<std::size_t>(f) * channels;
        for (unsigned c = 0; c < channels; ++c) {
            const std::uint32_t v =
                static_cast<std::uint32_t>(in[layout.from[c]]) << shift;
            for (std::uint32_t b = 0; b < container; ++b) {
                *out++ = static_cast<std::uint8_t>((v >> (b * 8)) & 0xffu);
            }
        }
    }
}

/// Decodes the next packet into `d->ready`. False at the end of the track or on
/// a packet this decoder will not accept.
bool fill(MpDecoder* d) noexcept
{
    while (d->next_packet < d->track.packets.size()) {
        const mp::mp4::Packet& p = d->track.packets[d->next_packet++];
        if (p.size == 0 || p.size > (16u << 20)) {
            log_fmt(MP_LOG_ERROR, "%s: implausible packet size %u", d->path.c_str(), p.size);
            return false;
        }
        d->packet.resize(p.size);
        if (_fseeki64(d->fp, static_cast<std::int64_t>(p.offset), SEEK_SET) != 0 ||
            std::fread(d->packet.data(), 1, p.size, d->fp) != p.size) {
            log_fmt(MP_LOG_ERROR, "%s: packet %zu is not where the sample table says",
                    d->path.c_str(), d->next_packet - 1);
            return false;
        }

        const std::uint32_t frames =
            d->codec.decode(d->packet.data(), d->packet.size(), d->decoded.data());
        if (frames == 0) {
            log_fmt(MP_LOG_ERROR, "%s: packet %zu: %s", d->path.c_str(), d->next_packet - 1,
                    d->codec.error());
            return false;
        }

        if (d->skip >= frames) {
            d->skip -= frames;
            continue;
        }

        pack(d, frames);
        if (d->skip != 0) {
            const std::size_t stride =
                static_cast<std::size_t>(d->codec.channels()) * d->container;
            d->ready_at = static_cast<std::size_t>(d->skip) * stride;
            d->skip = 0;
        }
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ vtable

MpResult MP_CALL decoder_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                               std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || bytes < 12) {
        return MP_OK;
    }
    if (std::memcmp(head + 4, "ftyp", 4) != 0) {
        return MP_OK;
    }

    // 100 for every MP4, not only the ones that show "alac" in the first four
    // kilobytes -- because whether they do is the muxer's choice and nothing to
    // do with us. FFmpeg puts `moov` after the audio and refalac puts it before,
    // so scoring on the visible bytes would make the chosen decoder depend on
    // which program wrote the file, which is the kind of behaviour that is
    // impossible to reason about later.
    //
    // Looking first is cheap: `open` walks the top-level boxes, reads one, and
    // says "no ALAC track in this file" in about the time it takes to seek
    // twice. The host then moves on to decode_mf, which is what should have the
    // AAC anyway.
    *out_score = 100;
    return MP_OK;
}

MpResult MP_CALL decoder_open(const char* path, MpDecoder** out) noexcept
{
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    MpDecoder* d = new (std::nothrow) MpDecoder();
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->path = path;

    d->fp = open_utf8(path);
    if (d->fp == nullptr) {
        delete d;
        return MP_ERR_IO;
    }

    std::vector<std::uint8_t> moov;
    if (!read_moov(d->fp, moov)) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    const char* why = "";
    if (!mp::mp4::parse_moov(moov.data(), moov.size(), d->track, &why) ||
        d->track.codec != mp::mp4::k_codec_alac) {
        // Not an error worth a warning: most MP4 files are not ALAC, and saying
        // so at debug level is what lets the host move on quietly.
        log_fmt(MP_LOG_DEBUG, "%s: %s", path, why);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_UNSUPPORTED;
    }

    mp::alac::Config cfg;
    if (!mp::alac::parse_config(d->track.config.data(), d->track.config.size(), cfg)) {
        log_fmt(MP_LOG_WARN, "%s: the ALAC magic cookie is not one this decoder accepts", path);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }
    if (!d->codec.init(cfg)) {
        log_fmt(MP_LOG_WARN, "%s: %s", path, d->codec.error());
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }

    d->container = container_for(cfg.bit_depth);
    d->shift = d->container * 8u - cfg.bit_depth;

    const mp::alac::ChannelLayout& layout = mp::alac::layout_for(cfg.channels);
    d->format.sample_rate = cfg.sample_rate;
    d->format.channels = cfg.channels;
    d->format.channel_mask = layout.mask;
    d->format.sample_type = sample_type_for(d->container, cfg.bit_depth);
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = cfg.bit_depth;

    if (d->format.sample_type == MP_SAMPLE_NONE) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }

    d->decoded.assign(static_cast<std::size_t>(cfg.frame_length) * cfg.channels, 0);

    *out = d;
    return MP_OK;
}

MpResult MP_CALL decoder_get_format(MpDecoder* d, MpFormat* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = d->format;
    return MP_OK;
}

MpResult MP_CALL decoder_get_length(MpDecoder* d, std::uint64_t* out_frames) noexcept
{
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_frames = d->track.total_frames;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
{
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t stride = static_cast<std::size_t>(d->format.channels) * d->container;
    if (stride == 0) {
        return MP_ERR_INTERNAL;
    }

    std::uint8_t* out = static_cast<std::uint8_t*>(dst);
    std::size_t room = (dst_bytes / stride) * stride;
    std::size_t done = 0;

    while (done < room) {
        if (d->ready_at >= d->ready.size()) {
            if (!fill(d)) {
                break;
            }
            continue;
        }
        std::size_t chunk = d->ready.size() - d->ready_at;
        if (chunk > room - done) {
            chunk = room - done;
        }
        std::memcpy(out + done, d->ready.data() + d->ready_at, chunk);
        d->ready_at += chunk;
        done += chunk;
    }

    // The sample table says how long the track is; the last packet is a whole
    // ALAC frame regardless, so anything past that length is padding the file
    // never claimed to contain.
    const std::uint64_t limit = d->track.total_frames;
    if (limit != 0) {
        const std::uint64_t have = d->frames_out + done / stride;
        if (have > limit) {
            const std::uint64_t excess = have - limit;
            const std::size_t trim = static_cast<std::size_t>(excess) * stride;
            done = trim >= done ? 0 : done - trim;
        }
    }
    d->frames_out += done / stride;

    *out_bytes = done;
    return done == 0 ? MP_END : MP_OK;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    const std::uint32_t per = d->codec.frame_length();
    if (per == 0) {
        return MP_ERR_INTERNAL;
    }
    const std::uint64_t packet = frame / per;
    if (packet > d->track.packets.size()) {
        return MP_ERR_INVALID;
    }
    d->next_packet = static_cast<std::size_t>(packet);
    d->skip = frame - packet * per;
    d->ready.clear();
    d->ready_at = 0;
    d->frames_out = frame;
    return MP_OK;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->fp != nullptr) {
        std::fclose(d->fp);
    }
    delete d;
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

const MpDecoderVtbl g_decoder_vtbl = {
    /* size       */ sizeof(MpDecoderVtbl),
    /* reserved   */ 0,
    /* probe      */ &decoder_probe,
    /* open       */ &decoder_open,
    /* get_format */ &decoder_get_format,
    /* get_length */ &decoder_get_length,
    /* read       */ &decoder_read,
    /* seek       */ &decoder_seek,
    /* close      */ &decoder_close,
};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DECODER,
    /* priority    */ 115, // above decode_mf, which gets ALAC channel order wrong
    /* id          */ "decode_alac",
    /* name        */ "ALAC (written here, not vendored)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_decoder_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi_version)
{
    if (host_abi_version != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC-LC, decoded by this project rather than by anything it links to.
//
// aac.hpp has the argument. The short version is that every library available
// produced the wrong *thing* rather than a wrong sound: Media Foundation starts
// every track 21 ms late and refuses 8 kHz and 7.1, FAAD2 discards two frames
// where the file says one, and libxaac emits integers. What this module adds on
// top of the codec is the other half of that -- the container's gapless edit,
// read from `elst`, and the channel order AAC's elements are not already in.

#include "aac.hpp"

#include <mp4.hpp>

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

/// `moov` is a top-level box and where it sits is the muxer's choice, so the top
/// level is walked by header alone and only the box wanted is read.
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
        if (std::memcmp(header + 4, "moov", 4) == 0) {
            const std::uint64_t body = size - static_cast<std::uint64_t>(header_bytes);
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

/// An ADTS header: the same configuration an MP4 keeps in `esds`, repeated in
/// front of every frame because a raw stream has nowhere else to put it.
struct Adts {
    unsigned object_type = 0;
    unsigned rate_index = 0;
    unsigned channel_config = 0;
    unsigned header_bytes = 0;
    unsigned frame_bytes = 0;
};

bool parse_adts(const std::uint8_t* h, std::size_t bytes, Adts& out) noexcept
{
    if (bytes < 7) {
        return false;
    }
    // Syncword, and the layer field: 0xFF followed by set bits is not rare, and
    // layer must be 00 for ADTS.
    if (h[0] != 0xFFu || (h[1] & 0xF0u) != 0xF0u || (h[1] & 0x06u) != 0) {
        return false;
    }
    const bool protection_absent = (h[1] & 1u) != 0;
    out.object_type = ((h[2] >> 6) & 0x3u) + 1u;
    out.rate_index = (h[2] >> 2) & 0xFu;
    out.channel_config = static_cast<unsigned>(((h[2] & 1u) << 2) | ((h[3] >> 6) & 0x3u));
    out.frame_bytes = (static_cast<unsigned>(h[3] & 0x3u) << 11) |
                      (static_cast<unsigned>(h[4]) << 3) |
                      (static_cast<unsigned>(h[5]) >> 5);
    const unsigned blocks = static_cast<unsigned>(h[6] & 0x3u) + 1u;
    out.header_bytes = protection_absent ? 7u : 9u;
    if (out.rate_index > 12 || out.frame_bytes <= out.header_bytes || blocks != 1) {
        return false;
    }
    return true;
}

} // namespace

struct MpDecoder {
    FILE* fp = nullptr;
    mp::aac::Config cfg;
    mp::aac::Decoder codec;
    MpFormat format{};
    mp::aac::ChannelLayout layout{};
    bool permute = false;

    bool from_mp4 = false;
    mp::mp4::AudioTrack track;
    std::size_t next_packet = 0;
    std::int64_t adts_at = 0; ///< byte offset of the next ADTS frame

    std::vector<std::uint8_t> packet;
    std::vector<float> ready;
    std::size_t ready_at = 0;

    std::uint64_t skip = 0;
    std::uint64_t emitted = 0;
    std::uint64_t limit = 0;
    bool ended = false;
    std::string path;
};

namespace {

/// Decodes one packet into `d->ready`: WAVE channel order, gapless edit applied.
bool fill(MpDecoder* d) noexcept
{
    while (!d->ended) {
        std::size_t size = 0;
        std::int64_t offset = 0;

        if (d->from_mp4) {
            if (d->next_packet >= d->track.packets.size()) {
                d->ended = true;
                return false;
            }
            const mp::mp4::Packet& p = d->track.packets[d->next_packet++];
            if (p.size == 0 || p.size > (8u << 20)) {
                d->ended = true;
                return false;
            }
            size = p.size;
            offset = static_cast<std::int64_t>(p.offset);
        } else {
            std::uint8_t header[9];
            if (_fseeki64(d->fp, d->adts_at, SEEK_SET) != 0 ||
                std::fread(header, 1, sizeof(header), d->fp) < 7) {
                d->ended = true;
                return false;
            }
            Adts adts;
            if (!parse_adts(header, sizeof(header), adts)) {
                d->ended = true;
                return false;
            }
            offset = d->adts_at + adts.header_bytes;
            size = adts.frame_bytes - adts.header_bytes;
            d->adts_at += adts.frame_bytes;
        }

        d->packet.resize(size);
        if (_fseeki64(d->fp, offset, SEEK_SET) != 0 ||
            std::fread(d->packet.data(), 1, size, d->fp) != size) {
            log_fmt(MP_LOG_ERROR, "%s: a packet is not where the file says", d->path.c_str());
            d->ended = true;
            return false;
        }

        if (!d->codec.decode_frame(d->packet.data(), d->packet.size())) {
            log_fmt(MP_LOG_ERROR, "%s: %s", d->path.c_str(), d->codec.error());
            d->ended = true;
            return false;
        }

        // Zero until decoder_open has settled the layout, which for a raw ADTS
        // stream in configuration 0 takes a decoded frame -- and that frame
        // comes through here.
        const unsigned channels = d->codec.channels();
        if (d->format.channels != 0 && channels != d->format.channels) {
            log_fmt(MP_LOG_ERROR, "%s changed from %u channels to %u mid-stream",
                    d->path.c_str(), d->format.channels, channels);
            d->ended = true;
            return false;
        }

        std::uint64_t frames = mp::aac::k_frame_len;
        std::uint64_t first = 0;

        // The encoder delay the edit list named, dropped before anything is
        // handed out, and the padding at the other end likewise. This is the
        // whole of what separates this module from the OS decoder.
        if (d->skip >= frames) {
            d->skip -= frames;
            continue;
        }
        if (d->skip != 0) {
            first = d->skip;
            frames -= d->skip;
            d->skip = 0;
        }
        if (d->limit != 0) {
            if (d->emitted >= d->limit) {
                d->ended = true;
                return false;
            }
            const std::uint64_t room = d->limit - d->emitted;
            if (frames > room) {
                frames = room;
            }
        }

        d->ready.resize(static_cast<std::size_t>(frames) * channels);
        d->ready_at = 0;
        for (std::uint64_t n = 0; n < frames; ++n) {
            float* out = d->ready.data() + n * channels;
            for (unsigned c = 0; c < channels; ++c) {
                const unsigned src = d->permute ? d->layout.from[c] : c;
                out[c] = d->codec.pcm(src)[first + n];
            }
        }
        d->emitted += frames;
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
    // Every MP4 is looked at, for the same reason decode_alac looks at every
    // MP4: whether `mp4a` is visible in the first four kilobytes depends on
    // where the muxer put `moov` and on nothing about the file. Declining a
    // non-AAC one costs two seeks.
    if (std::memcmp(head + 4, "ftyp", 4) == 0) {
        *out_score = 100;
        return MP_OK;
    }
    Adts adts;
    if (parse_adts(head, bytes, adts)) {
        *out_score = 100;
    }
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

    std::uint8_t probe[16] = {};
    const std::size_t got = std::fread(probe, 1, sizeof(probe), d->fp);
    std::rewind(d->fp);

    if (got >= 12 && std::memcmp(probe + 4, "ftyp", 4) == 0) {
        std::vector<std::uint8_t> moov;
        const char* why = "";
        if (!read_moov(d->fp, moov) ||
            !mp::mp4::parse_moov(moov.data(), moov.size(), d->track, &why) ||
            d->track.codec != mp::mp4::k_codec_mp4a) {
            log_fmt(MP_LOG_DEBUG, "%s: %s", path, why[0] != '\0' ? why : "not an AAC track");
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        if (!mp::aac::parse_asc(d->track.config.data(), d->track.config.size(), d->cfg)) {
            log_fmt(MP_LOG_DEBUG,
                    "%s: not an AAC-LC AudioSpecificConfig -- SBR and HE-AAC go to "
                    "decode_ffmpeg",
                    path);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->from_mp4 = true;
        d->skip = d->track.skip_frames;
        d->limit = d->track.play_frames;
        log_fmt(MP_LOG_DEBUG, "%s: edit list says skip %llu frames and play %llu", path,
                static_cast<unsigned long long>(d->skip),
                static_cast<unsigned long long>(d->limit));
    } else {
        Adts adts;
        if (got < 7 || !parse_adts(probe, got, adts)) {
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        // A raw stream carries no edit list, so there is nothing to trim and
        // nothing here invents any: the encoder's delay is in the audio and the
        // file does not say how much.
        d->cfg.object_type = adts.object_type;
        d->cfg.rate_index = adts.rate_index;
        d->cfg.sample_rate = mp::aac::rate_for_index(adts.rate_index);
        d->cfg.channel_config = adts.channel_config;
        d->cfg.frame_960 = false;
        if (d->cfg.object_type != 2 || d->cfg.sample_rate == 0) {
            log_fmt(MP_LOG_DEBUG, "%s is ADTS but not AAC-LC", path);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->adts_at = 0;
    }

    if (!d->codec.init(d->cfg)) {
        log_fmt(MP_LOG_WARN, "%s: %s", path, d->codec.error());
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }

    // The codec knows its own layout: a table for configurations 1 to 12, and
    // for configuration 0 whatever program config element the
    // AudioSpecificConfig carried.
    d->layout = d->codec.layout();
    if (d->layout.count == 0 && d->cfg.channel_config == 0) {
        // Which leaves raw ADTS, whose header has a configuration field and no
        // room for a PCE: there the element arrives inside the frames, so one
        // frame has to be decoded before the format can be reported and the
        // decoder then starts again. FFmpeg writes configuration 0 for
        // 7.1(wide), so this is not a corner nobody reaches.
        if (fill(d)) {
            d->layout = d->codec.layout();
            d->next_packet = 0;
            d->adts_at = 0;
            d->ready.clear();
            d->ready_at = 0;
            d->emitted = 0;
            d->ended = false;
            d->skip = d->from_mp4 ? d->track.skip_frames : 0;
            d->codec.init(d->cfg);
        }
    }
    if (d->layout.count == 0) {
        log_fmt(MP_LOG_WARN, "%s: channel configuration %u has no layout here", path,
                d->cfg.channel_config);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }
    for (unsigned c = 0; c < d->layout.count; ++c) {
        if (d->layout.from[c] != c) {
            d->permute = true;
        }
    }

    d->format.sample_rate = d->cfg.sample_rate;
    d->format.channels = d->layout.count;
    d->format.channel_mask = d->layout.mask;
    d->format.sample_type = MP_SAMPLE_F32;
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = 0;

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
    *out_frames = d->limit != 0 ? d->limit : d->track.total_frames;
    return MP_OK;
}

MpResult MP_CALL decoder_read(MpDecoder* d, void* dst, std::size_t dst_bytes,
                              std::size_t* out_bytes) noexcept
{
    if (d == nullptr || dst == nullptr || out_bytes == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_bytes = 0;

    const std::size_t stride = static_cast<std::size_t>(d->format.channels) * sizeof(float);
    if (stride == 0) {
        return MP_ERR_INTERNAL;
    }
    const std::size_t room = (dst_bytes / stride) * stride;
    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t done = 0;

    while (done < room) {
        const std::size_t have = (d->ready.size() - d->ready_at) * sizeof(float);
        if (have == 0) {
            if (!fill(d)) {
                break;
            }
            continue;
        }
        std::size_t chunk = have < room - done ? have : room - done;
        std::memcpy(out + done, d->ready.data() + d->ready_at, chunk);
        d->ready_at += chunk / sizeof(float);
        done += chunk;
    }

    *out_bytes = done;
    return done == 0 ? MP_END : MP_OK;
}

MpResult MP_CALL decoder_seek(MpDecoder* d, std::uint64_t frame) noexcept
{
    if (d == nullptr) {
        return MP_ERR_INVALID;
    }
    if (frame != 0) {
        // AAC's transform is lapped, so frame N cannot be produced without
        // N-1's tail. Seeking properly means decoding a frame of warm-up before
        // the target and throwing it away, which is not written yet -- and
        // claiming to have seeked when the first frame after it would be wrong
        // is worse than saying no.
        return MP_ERR_UNSUPPORTED;
    }
    if (!d->codec.init(d->cfg)) {
        return MP_ERR_INTERNAL;
    }
    d->next_packet = 0;
    d->adts_at = 0;
    d->ready.clear();
    d->ready_at = 0;
    d->emitted = 0;
    d->ended = false;
    d->skip = d->from_mp4 ? d->track.skip_frames : 0;
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
    /* priority    */ 108, // below decode_alac's look at the same MP4, above FFmpeg's
    /* id          */ "decode_aac",
    /* name        */ "AAC-LC (written here, not vendored)",
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

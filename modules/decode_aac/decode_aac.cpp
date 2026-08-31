// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAC, through FAAD2 -- and, like decode_mp3, not because AAC needed a better
// decoder.
//
// Media Foundation does not implement gapless metadata. Measured against the
// WAV that was encoded, it starts every AAC track 1024 frames -- 21.3 ms --
// late and leaves the encoder's padding on the end, because it ignores the
// `elst` edit that says how much of the decoded audio the file actually claims.
// It also refuses 8 kHz AAC and 7.1 AAC outright. This module reads the edit
// list, and FAAD2 reads the rest.
//
// **This module is not finished, and its priority says so.**
//
// The decoding is right: against FFmpeg on the same file, over the region both
// produce, the two agree to 134.45 dB with a maximum sample difference of
// 6 x 10^-8. That is float rounding, and it is a better result than AAC even
// requires -- ISO/IEC 14496-4 defines conformance as an RMS error bound, not as
// an identity, so two implementations are *specified* to be allowed to differ.
//
// The placement is wrong. Measured against the WAV that was encoded:
//
//   ffmpeg   96000 frames, ff[i] == src[i]
//   FAAD2    95232 frames, fa[i-1024] == src[i]
//
// FAAD2 answers the first packet with no samples and its output then begins
// 1024 frames into the audio, so the first 1024 frames of every track are
// missing and the tail is short by the final packet. Forcing the edit list skip
// to zero does not recover them: the frames are not produced. Until that is
// understood, this module sits at priority 40 -- below decode_mf -- so it is
// reachable with  and is never chosen for playback.
//
// Two more known gaps, both FAAD2's rather than this file's:
//   * a mono track comes back as stereo, because ffmpeg writes an SBR signalling
//     extension into the AudioSpecificConfig and FAAD2 takes the parametric
//     stereo path. dontUpSampleImplicitSBR did not change it.
//   * 7.1 is refused outright at NeAACDecInit2.
//
// Licence note: FAAD2 is GPL-2.0-or-later, which is compatible with this
// project's GPL-3.0-or-later. Its licence carries an attribution requirement
// under GPLv2 section 2c, quoted in modules/decode_aac/CMakeLists.txt and
// reproduced by `mediaperch-probe modules`.

#include <mp4.hpp>

#include <mediaperch/module.h>

#include <neaacdec.h>

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

/// The WAVE speaker bit for one of FAAD2's channel positions.
std::uint32_t speaker_bit(unsigned position) noexcept
{
    switch (position) {
    case FRONT_CHANNEL_LEFT: return 0x1u;
    case FRONT_CHANNEL_RIGHT: return 0x2u;
    case FRONT_CHANNEL_CENTER: return 0x4u;
    case LFE_CHANNEL: return 0x8u;
    case BACK_CHANNEL_LEFT: return 0x10u;
    case BACK_CHANNEL_RIGHT: return 0x20u;
    case BACK_CHANNEL_CENTER: return 0x100u;
    case SIDE_CHANNEL_LEFT: return 0x200u;
    case SIDE_CHANNEL_RIGHT: return 0x400u;
    default: return 0u;
    }
}

/// `moov` is a top-level box and where it sits is the muxer's choice, so the
/// top level is walked by header alone. Only the box wanted is read.
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

/// Whether these bytes open an ADTS frame. Both MPEG-4 and MPEG-2 spellings,
/// and the layer field checked, because 0xFF followed by set bits is not rare.
bool is_adts(const std::uint8_t* h) noexcept
{
    return h[0] == 0xFFu && (h[1] & 0xF6u) == 0xF0u;
}

} // namespace

struct MpDecoder {
    FILE* fp = nullptr;
    NeAACDecHandle faad = nullptr;

    bool from_mp4 = false;
    mp::mp4::AudioTrack track;
    std::size_t next_packet = 0;

    // ADTS reads through a sliding window rather than loading the file: a raw
    // AAC stream has no index, so the only way through it is forwards.
    std::vector<std::uint8_t> window;
    std::size_t window_at = 0;

    MpFormat format{};
    unsigned channels = 0;
    std::uint8_t order[64] = {}; ///< for each WAVE slot, the FAAD2 channel
    bool permute = false;

    std::vector<float> ready; ///< one decoded frame, already permuted
    std::size_t ready_at = 0;

    std::uint64_t skip = 0;   ///< frames of encoder delay still to discard
    std::uint64_t emitted = 0;
    std::uint64_t limit = 0;  ///< frames the edit list says to play, 0 = all
    bool ended = false;
    std::uint64_t frames_seen = 0;
    std::uint64_t swallowed = 0; ///< nominal frames FAAD2 consumed without output
    std::string path;
};

namespace {

/// Fills `window` with at least `want` bytes if the file has them.
bool refill(MpDecoder* d, std::size_t want) noexcept
{
    if (d->window.size() - d->window_at >= want) {
        return true;
    }
    d->window.erase(d->window.begin(), d->window.begin() + static_cast<std::ptrdiff_t>(d->window_at));
    d->window_at = 0;
    const std::size_t target = want > 65536 ? want : 65536;
    const std::size_t had = d->window.size();
    d->window.resize(target);
    const std::size_t got = std::fread(d->window.data() + had, 1, target - had, d->fp);
    d->window.resize(had + got);
    return d->window.size() >= want;
}

/// The nominal frame count of one packet, which is what the edit list counts in.
///
/// Every packet in a track has the same duration except, sometimes, the last, so
/// the total from `stts` over the count from `stsz` is the duration of a whole
/// one. Deriving it beats assuming 1024, which is right for AAC-LC and wrong for
/// anything carrying SBR.
std::uint64_t frames_in_packet(const MpDecoder* d) noexcept
{
    const std::size_t count = d->track.packets.size();
    if (count == 0 || d->track.total_frames == 0) {
        return 1024;
    }
    const std::uint64_t whole = d->track.total_frames / count;
    return whole == 0 ? 1024 : whole;
}

/// Builds the WAVE-order permutation from what FAAD2 says this frame contains.
///
/// The table is not hard-coded because it does not have to be: FAAD2 reports a
/// speaker position per output channel, so the mapping comes from the file
/// rather than from an assumption about what a six-channel AAC "usually" is.
void build_order(MpDecoder* d, const NeAACDecFrameInfo& info) noexcept
{
    d->channels = info.channels;
    // The format is taken from a frame that decoded, never from NeAACDecInit2.
    // Init reports what the AudioSpecificConfig *allows*: for a mono file it
    // answers two, and for HE-AAC it answers the rate before SBR doubles it.
    // Neither is what comes out. Only a decoded frame knows.
    d->format.channels = info.channels;
    if (info.samplerate != 0) {
        d->format.sample_rate = static_cast<std::uint32_t>(info.samplerate);
    }
    d->permute = false;
    d->format.channel_mask = 0;
    for (unsigned c = 0; c < 64; ++c) {
        d->order[c] = static_cast<std::uint8_t>(c);
    }
    if (info.channels <= 2 || info.channels > 32) {
        return; // mono and stereo agree with everyone, as elsewhere in this tree
    }

    std::uint32_t mask = 0;
    for (unsigned c = 0; c < info.channels; ++c) {
        const std::uint32_t bit = speaker_bit(info.channel_position[c]);
        if (bit == 0 || (mask & bit) != 0) {
            log_fmt(MP_LOG_WARN,
                    "%s: channel %u has a speaker position this module cannot place; "
                    "leaving the order alone",
                    d->path.c_str(), c);
            return;
        }
        mask |= bit;
    }

    // WAVE order is ascending speaker bit. Walk the bits, and for each find the
    // FAAD2 channel that claims it.
    unsigned slot = 0;
    for (unsigned b = 0; b < 32; ++b) {
        const std::uint32_t bit = 1u << b;
        if ((mask & bit) == 0) {
            continue;
        }
        for (unsigned c = 0; c < info.channels; ++c) {
            if (speaker_bit(info.channel_position[c]) == bit) {
                d->order[slot++] = static_cast<std::uint8_t>(c);
                break;
            }
        }
    }
    d->format.channel_mask = mask;
    for (unsigned c = 0; c < info.channels; ++c) {
        if (d->order[c] != c) {
            d->permute = true;
        }
    }
}

/// Decodes one AAC frame into `d->ready`, applying the gapless edit.
bool fill(MpDecoder* d) noexcept
{
    while (!d->ended) {
        NeAACDecFrameInfo info{};
        void* pcm = nullptr;

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
            d->window.resize(p.size);
            if (_fseeki64(d->fp, static_cast<std::int64_t>(p.offset), SEEK_SET) != 0 ||
                std::fread(d->window.data(), 1, p.size, d->fp) != p.size) {
                log_fmt(MP_LOG_ERROR, "%s: packet %zu is not where the sample table says",
                        d->path.c_str(), d->next_packet - 1);
                d->ended = true;
                return false;
            }
            pcm = NeAACDecDecode(d->faad, &info, d->window.data(), static_cast<unsigned long>(p.size));
        } else {
            if (!refill(d, 8192) && d->window.size() - d->window_at < 8) {
                d->ended = true;
                return false;
            }
            pcm = NeAACDecDecode(d->faad, &info, d->window.data() + d->window_at,
                                 static_cast<unsigned long>(d->window.size() - d->window_at));
            if (info.bytesconsumed == 0) {
                d->ended = true;
                return false;
            }
            d->window_at += info.bytesconsumed;
        }

        if (info.error != 0) {
            log_fmt(MP_LOG_ERROR, "FAAD2: %s in %s", NeAACDecGetErrorMessage(info.error),
                    d->path.c_str());
            d->ended = true;
            return false;
        }
        // FAAD2 answers the first packet with no samples at all: it swallows the
        // decoder's own priming frame rather than handing it out. The MP4 edit
        // list counts from a decoder that does hand it out, so applying
        // `media_time` on top of what FAAD2 already dropped removes the audio
        // twice -- measurable as starting 1024 frames later than FFmpeg on the
        // same file, with the tail correspondingly short.
        //
        // What is subtracted is what was actually swallowed, derived from the
        // sample table rather than assumed to be 1024: a packet is 1024 frames
        // for AAC-LC and 2048 with SBR, and this way nothing has to know which.
        if (pcm == nullptr || info.samples == 0 || info.channels == 0) {
            if (d->from_mp4 && d->frames_seen == 0) {
                const std::uint64_t nominal = frames_in_packet(d);
                d->swallowed += nominal;
                d->skip = d->skip > nominal ? d->skip - nominal : 0;
            }
            continue; // a priming frame; FAAD2 emits these at the start
        }
        ++d->frames_seen;

        if (info.channels != d->channels || d->format.sample_rate != info.samplerate) {
            build_order(d, info);
        }

        const unsigned channels = info.channels;
        std::uint64_t frames = info.samples / channels;
        const float* src = static_cast<const float*>(pcm);

        // The encoder delay the edit list named, discarded before anything is
        // handed out. This is the whole difference between this module and the
        // OS decoder.
        if (d->skip >= frames) {
            d->skip -= frames;
            continue;
        }
        if (d->skip != 0) {
            src += d->skip * channels;
            frames -= d->skip;
            d->skip = 0;
        }

        // And the padding at the other end, which the edit list also names.
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
        if (d->permute) {
            for (std::uint64_t f = 0; f < frames; ++f) {
                const float* in = src + f * channels;
                float* out = d->ready.data() + f * channels;
                for (unsigned c = 0; c < channels; ++c) {
                    out[c] = in[d->order[c]];
                }
            }
        } else {
            std::memcpy(d->ready.data(), src,
                        static_cast<std::size_t>(frames) * channels * sizeof(float));
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
    if (std::memcmp(head + 4, "ftyp", 4) == 0) {
        // Like decode_alac: every MP4 is looked at, because whether `mp4a` is
        // visible in the first four kilobytes depends on where the muxer put
        // `moov` and not on anything about the file.
        *out_score = 100;
        return MP_OK;
    }
    if (is_adts(head)) {
        *out_score = 100;
        return MP_OK;
    }
    if (std::memcmp(head, "ADIF", 4) == 0) {
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

    d->faad = NeAACDecOpen();
    if (d->faad == nullptr) {
        std::fclose(d->fp);
        delete d;
        return MP_ERR_NO_MEMORY;
    }

    // Float, because AAC's inverse transform produces real numbers and int16 is
    // a quantisation. FAAD2 will do either; nothing here converts.
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(d->faad);
    config->outputFormat = FAAD_FMT_FLOAT;
    config->downMatrix = 0; // never fold multichannel down to stereo
    config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(d->faad, config);

    unsigned long rate = 0;
    unsigned char channels = 0;

    std::uint8_t probe[16] = {};
    std::fread(probe, 1, sizeof(probe), d->fp);
    std::rewind(d->fp);
    const bool looks_mp4 = std::memcmp(probe + 4, "ftyp", 4) == 0;

    if (looks_mp4) {
        std::vector<std::uint8_t> moov;
        const char* why = "";
        if (!read_moov(d->fp, moov) ||
            !mp::mp4::parse_moov(moov.data(), moov.size(), d->track, &why) ||
            d->track.codec != mp::mp4::k_codec_mp4a) {
            log_fmt(MP_LOG_DEBUG, "%s: %s", path, why[0] != '\0' ? why : "not an AAC track");
            NeAACDecClose(d->faad);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        if (NeAACDecInit2(d->faad, d->track.config.data(),
                          static_cast<unsigned long>(d->track.config.size()), &rate,
                          &channels) < 0) {
            log_fmt(MP_LOG_WARN, "%s: FAAD2 will not take this AudioSpecificConfig", path);
            NeAACDecClose(d->faad);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_FORMAT;
        }
        d->from_mp4 = true;
        d->skip = d->track.skip_frames;
        d->limit = d->track.play_frames;
    } else {
        if (!refill(d, 8192) && d->window.empty()) {
            NeAACDecClose(d->faad);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        const long consumed =
            NeAACDecInit(d->faad, d->window.data(),
                         static_cast<unsigned long>(d->window.size()), &rate, &channels);
        if (consumed < 0) {
            log_fmt(MP_LOG_DEBUG, "%s is not a raw AAC stream FAAD2 recognises", path);
            NeAACDecClose(d->faad);
            std::fclose(d->fp);
            delete d;
            return MP_ERR_UNSUPPORTED;
        }
        d->window_at = static_cast<std::size_t>(consumed);
    }

    if (rate == 0 || channels == 0) {
        NeAACDecClose(d->faad);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }

    d->channels = channels;
    d->format.sample_rate = static_cast<std::uint32_t>(rate);
    d->format.channels = channels;
    d->format.channel_mask = 0;
    d->format.sample_type = MP_SAMPLE_F32;
    d->format.encoding = MP_ENCODING_PCM;
    d->format.valid_bits = 0;
    for (unsigned c = 0; c < 64; ++c) {
        d->order[c] = static_cast<std::uint8_t>(c);
    }

    if (d->from_mp4) {
        log_fmt(MP_LOG_DEBUG, "%s: edit list says skip %llu frames and play %llu", path,
                static_cast<unsigned long long>(d->skip),
                static_cast<unsigned long long>(d->limit));
    }

    // Decode one frame here rather than at the first read, so that get_format
    // describes what will actually come out. It is also the only way to find
    // out: see build_order.
    if (!fill(d)) {
        log_fmt(MP_LOG_WARN, "%s: no AAC frame in this file decoded to anything", path);
        NeAACDecClose(d->faad);
        std::fclose(d->fp);
        delete d;
        return MP_ERR_FORMAT;
    }

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
    if (frame == 0) {
        // Rewinding is the one seek that can be done honestly without a
        // sample-accurate index: tear the decoder down and start again.
        NeAACDecClose(d->faad);
        d->faad = NeAACDecOpen();
        if (d->faad == nullptr) {
            return MP_ERR_NO_MEMORY;
        }
        NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(d->faad);
        config->outputFormat = FAAD_FMT_FLOAT;
        config->downMatrix = 0;
        NeAACDecSetConfiguration(d->faad, config);

        unsigned long rate = 0;
        unsigned char channels = 0;
        if (d->from_mp4) {
            if (NeAACDecInit2(d->faad, d->track.config.data(),
                              static_cast<unsigned long>(d->track.config.size()), &rate,
                              &channels) < 0) {
                return MP_ERR_IO;
            }
            d->next_packet = 0;
            d->skip = d->track.skip_frames;
        } else {
            std::rewind(d->fp);
            d->window.clear();
            d->window_at = 0;
            if (!refill(d, 8192)) {
                return MP_ERR_IO;
            }
            const long consumed =
                NeAACDecInit(d->faad, d->window.data(),
                             static_cast<unsigned long>(d->window.size()), &rate, &channels);
            if (consumed < 0) {
                return MP_ERR_IO;
            }
            d->window_at = static_cast<std::size_t>(consumed);
        }
        d->ready.clear();
        d->ready_at = 0;
        d->emitted = 0;
        d->ended = false;
        return MP_OK;
    }
    return MP_ERR_UNSUPPORTED;
}

void MP_CALL decoder_close(MpDecoder* d) noexcept
{
    if (d == nullptr) {
        return;
    }
    if (d->faad != nullptr) {
        NeAACDecClose(d->faad);
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
    /* priority    */ 40, // BELOW decode_mf on purpose -- see the header comment
    /* id          */ "decode_aac",
    /* name        */ "AAC (FAAD2, INCOMPLETE; code from FAAD2 is copyright (c) Nero AG, www.nero.com)",
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

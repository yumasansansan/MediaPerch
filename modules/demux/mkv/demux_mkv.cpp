// SPDX-License-Identifier: GPL-3.0-or-later
//
// Matroska and WebM, on libebml and libmatroska: the reference implementations,
// from the people who define the format.
//
// **Matroska is the container that makes the v2 split pay for itself.** It
// carries FLAC, Vorbis, Opus, AAC, MP3, ALAC and PCM, and every one of those
// already has a codec module here -- so what this file does is identify streams
// and hand over packets, and seven formats arrive at once without a line of
// decoding. Under v1 an `.mka` went to FFmpeg entire, because "which decoder
// reads this file" has no answer for a container that could hold any of them.
//
// It is also the container that makes `MpStreamInfo` mean what it says. A
// Matroska file is genuinely several streams -- audio, video, subtitles, and
// more than one of each -- so `stream_count` is not decoration here, and the
// video and subtitle tracks are reported rather than hidden. §9's video path
// will ask for them; today the host picks an audio track and the rest are
// visible in `mediaperch-probe claims`.
//
// **Why libmatroska rather than an EBML parser written here.** EBML is a nested,
// self-describing, variable-length encoding, and a reader has to be right about
// lacing (three kinds of it), about elements of unknown size, about SimpleBlock
// versus BlockGroup, and about timestamp scaling -- a great deal of parser over
// a stranger's bytes. And this tree has already learned what happens when a
// hand-written parser stands in front of a reference implementation offering the
// same thing: see the top of `demux_flac.cpp`.
//
// What is left here is the part that is genuinely ours: mapping Matroska's codec
// ids onto MpCodec, and turning each codec's CodecPrivate into the configuration
// blob the ABI defines. Those are the two places a container and a codec have to
// agree, and they are exactly what a demuxer is for.

#include <mediaperch/module.h>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>

#include <matroska/KaxBlock.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxContexts.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTracks.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using namespace libebml;
using namespace libmatroska;

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

/// Matroska's default timestamp scale: one millisecond, in nanoseconds.
constexpr std::uint64_t k_default_scale = 1000000;

/// What a Matroska CodecID means here.
///
/// **The ids are strings and the mapping is a table**, which is the whole of
/// what §4 meant by "capability declaration is data". MP4 spells ALAC `alac`,
/// Matroska spells it `A_ALAC`, and Ogg puts an identification header in its
/// first page; two containers spelling one codec differently are not two
/// codecs, and this is where that is said once.
MpCodec codec_for(const std::string& id) noexcept
{
    if (id == "A_FLAC") {
        return MP_CODEC_FLAC;
    }
    if (id == "A_VORBIS") {
        return MP_CODEC_VORBIS;
    }
    if (id == "A_OPUS") {
        return MP_CODEC_OPUS;
    }
    if (id == "A_AAC" || id.rfind("A_AAC/", 0) == 0) {
        return MP_CODEC_AAC_LC;
    }
    if (id == "A_MPEG/L3") {
        return MP_CODEC_MP3;
    }
    if (id == "A_MPEG/L2") {
        return MP_CODEC_MP2;
    }
    if (id == "A_MPEG/L1") {
        return MP_CODEC_MP1;
    }
    if (id == "A_ALAC") {
        return MP_CODEC_ALAC;
    }
    // **Only the little-endian integer spelling becomes MP_CODEC_PCM.** The
    // other two are named as themselves so a file holding them is refused by
    // the codec lookup rather than played as noise: `codec_pcm` is a memcpy,
    // and a big-endian or float stream needs a conversion that no module here
    // performs. Naming them is what sends such a file to FFmpeg with a reason.
    if (id == "A_PCM/INT/LIT") {
        return MP_CODEC_PCM;
    }
    if (id == "A_AC3" || id.rfind("A_AC3/", 0) == 0) {
        return MP_CODEC_AC3;
    }
    if (id == "A_EAC3") {
        return MP_CODEC_EAC3;
    }
    if (id == "A_DTS" || id.rfind("A_DTS/", 0) == 0) {
        return MP_CODEC_DTS;
    }
    if (id == "A_WAVPACK4") {
        return MP_CODEC_WAVPACK;
    }
    if (id == "A_TTA1") {
        return MP_CODEC_TTA;
    }
    return MP_CODEC_UNKNOWN;
}

MpStreamKind kind_for(std::uint64_t track_type) noexcept
{
    switch (track_type) {
    case track_audio: return MP_STREAM_AUDIO;
    case track_video: return MP_STREAM_VIDEO;
    case track_subtitle: return MP_STREAM_SUBTITLE;
    default: return MP_STREAM_OTHER;
    }
}

/// Vorbis's three header packets, out of Matroska's xiph lacing and into the
/// framing the ABI defines.
///
/// **This is the one codec whose configuration does not fit in one blob**, so
/// every container invents a framing for the three headers. Matroska uses xiph
/// lacing -- a chain of bytes valued 255 that sum to each length, terminated by
/// a byte below 255 -- and this tree uses a u32 little-endian length in front of
/// each. Converting here is what lets `codec_vorbis` be the same module for Ogg
/// and for Matroska.
bool vorbis_config(const std::uint8_t* p, std::size_t bytes, std::vector<std::uint8_t>& out)
{
    if (p == nullptr || bytes < 3 || p[0] != 2) {
        return false; // stored as "number of headers minus one", so always 2
    }
    std::size_t at = 1;
    std::size_t lengths[3] = {0, 0, 0};
    for (std::size_t i = 0; i < 2; ++i) {
        std::size_t length = 0;
        for (;;) {
            if (at >= bytes) {
                return false;
            }
            length += p[at];
            if (p[at++] != 255) {
                break;
            }
        }
        lengths[i] = length;
    }
    if (at + lengths[0] + lengths[1] > bytes) {
        return false;
    }
    lengths[2] = bytes - at - lengths[0] - lengths[1];

    out.clear();
    for (std::size_t i = 0; i < 3; ++i) {
        const auto n = static_cast<std::uint32_t>(lengths[i]);
        for (int b = 0; b < 4; ++b) {
            out.push_back(static_cast<std::uint8_t>((n >> (8 * b)) & 0xFFu));
        }
        out.insert(out.end(), p + at, p + at + lengths[i]);
        at += lengths[i];
    }
    return true;
}

/// FLAC's STREAMINFO out of Matroska's CodecPrivate.
///
/// Matroska stores a whole native FLAC header there -- `fLaC` and every metadata
/// block -- and the ABI defines MP_CODEC_FLAC's blob as the STREAMINFO alone.
/// The same trim `demux_ogg` performs, for the same reason: a codec should not
/// be able to tell which container called it.
bool flac_config(const std::uint8_t* p, std::size_t bytes, std::vector<std::uint8_t>& out)
{
    if (p == nullptr || bytes < 4 + 4 + 34 || std::memcmp(p, "fLaC", 4) != 0) {
        return false;
    }
    std::size_t at = 4;
    for (int guard = 0; guard < 64; ++guard) {
        if (at + 4 > bytes) {
            return false;
        }
        const bool last = (p[at] & 0x80u) != 0;
        const unsigned type = p[at] & 0x7Fu;
        const std::size_t length = (static_cast<std::size_t>(p[at + 1]) << 16) |
                                   (static_cast<std::size_t>(p[at + 2]) << 8) |
                                   static_cast<std::size_t>(p[at + 3]);
        at += 4;
        if (at + length > bytes) {
            return false;
        }
        if (type == 0 && length >= 34) {
            out.assign(p + at, p + at + 34);
            return true;
        }
        at += length;
        if (last) {
            return false;
        }
    }
    return false;
}

/// The smallest container that holds `bits`, in bytes, or 0 if none does.
std::uint32_t container_for(std::uint32_t bits) noexcept
{
    if (bits == 0 || bits > 32) {
        return 0;
    }
    if (bits <= 16) {
        return 2;
    }
    if (bits <= 24) {
        return 3;
    }
    return 4;
}

MpSampleType sample_type_for(std::uint32_t container, std::uint32_t valid) noexcept
{
    if (valid == 0 || valid > container * 8) {
        return MP_SAMPLE_NONE;
    }
    switch (container) {
    case 2: return MP_SAMPLE_S16;
    case 3: return MP_SAMPLE_S24_PACKED;
    case 4: return valid <= 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
    default: return MP_SAMPLE_NONE;
    }
}

/// A Matroska track, as far as this module reads one.
/// Nanoseconds to frames, without going through a double and without
/// overflowing on a long file: the whole seconds first, then the remainder.
///
/// **Rounded to nearest, not truncated, and the difference is measured.** Every
/// one of these numbers was a frame count before the muxer wrote it, and a whole
/// number of frames rarely lands on a whole nanosecond: an MP3's 1105-frame
/// delay is 25056689.34 ns and ffmpeg writes 25056689, which truncates back to
/// 1104. Rounding returns the number the muxer started with. Vorbis's 128 and
/// 120 in the same file are off by the same one nanosecond.
std::uint64_t to_frames(std::uint64_t ns, std::uint32_t rate)
{
    // The remainder is under 10^9 and the rate under 2^21, so this stays inside
    // 64 bits at every rate FLAC and Matroska can state.
    const std::uint64_t rem = (ns % 1000000000ull) * rate;
    return ns / 1000000000ull * rate + (rem + 500000000ull) / 1000000000ull;
}

/// What a track's last block says about where its audio stops. Filled by
/// `find_tail`, which is where the two elements involved are explained.
struct Tail {
    std::uint64_t end_ns = 0;   ///< 0 when the last block did not say.
    std::uint64_t trim_ns = 0;  ///< 0 when it stated no padding, which is usual.
};

struct Track {
    std::uint64_t number = 0; ///< Matroska's own number, which is not the index
    MpStreamKind kind = MP_STREAM_OTHER;
    MpCodec codec = MP_CODEC_UNKNOWN;
    std::string codec_id;
    std::vector<std::uint8_t> config;
    MpFormat format{};
    bool is_default = false;
    /// CodecDelay in nanoseconds: the encoder's warm-up, which every container
    /// in this tree states differently and which `MpStreamInfo` states once.
    std::uint64_t codec_delay_ns = 0;
    /// What the last block says about where the audio stops -- see `find_tail`.
    /// Looked for once, on the first `stream_info`.
    Tail tail{};
    bool tail_looked_for = false;
};

} // namespace

struct MpDemux {
    std::unique_ptr<StdIOCallback> io;
    std::unique_ptr<EbmlStream> stream;
    std::unique_ptr<EbmlElement> segment;

    std::uint64_t timestamp_scale = k_default_scale;
    double duration_scaled = 0.0;
    std::vector<Track> tracks;
    std::size_t selected = 0;

    /// Where the clusters begin, so a seek can start over from a known point.
    std::uint64_t first_cluster = 0;
    /// Where the segment's data starts, which is what Cues count from.
    std::uint64_t segment_data = 0;

    /// The Cues, which is Matroska's own seek index: a timestamp and the
    /// position of the cluster that holds it. Sorted, and never empty -- the
    /// start of the audio is always in it.
    struct Cue {
        std::uint64_t ns = 0;
        std::uint64_t at = 0;
    };
    std::vector<Cue> cues;

    /// The cluster being walked, and the level-1 element that ended it.
    ///
    /// **libebml reports the end of a master element by handing back the next
    /// one**, with `upper` counting how many levels it belongs above. That
    /// element has been read and must not be read again, so it is kept here and
    /// used as the start of the next cluster. Skipping it -- which is what the
    /// first version of this file did -- throws away a whole cluster of audio.
    std::unique_ptr<EbmlElement> cluster;
    std::unique_ptr<EbmlElement> pending;
    std::uint64_t cluster_ts = 0; ///< unscaled
    int upper = 0;

    /// The block being handed out, a lace at a time. Matroska packs several
    /// frames into one block, so a block is not a packet.
    ///
    /// **Two of them, one owning and one pointing.** A SimpleBlock *is* the
    /// element; a Block sits inside a BlockGroup and belongs to it. Keeping the
    /// element that owns the frames alive and pointing at the block inside it
    /// covers both without copying a block -- and copying one was the first
    /// version of this, which silently produced a block with no frames in it.
    std::unique_ptr<EbmlElement> block_owner;
    KaxInternalBlock* block = nullptr;
    /// What the block said it lasts, in nanoseconds, and how much of the end
    /// of it is padding. Both are 0 for a SimpleBlock, which has nowhere to say
    /// either.
    std::uint64_t block_duration_ns = 0;
    std::uint64_t discard_ns = 0;
    unsigned next_lace = 0;

    std::uint64_t position = 0; ///< the next packet's first sample
    bool ended = false;

    [[nodiscard]] const Track& track() const { return tracks[selected]; }
};

namespace {

/// Nanoseconds to frames of the selected track.
std::uint64_t frames_of(const MpDemux* d, std::uint64_t ns) noexcept
{
    const std::uint64_t rate = d->track().format.sample_rate;
    if (rate == 0) {
        return 0;
    }
    return ns / 1000000000ull * rate + (ns % 1000000000ull) * rate / 1000000000ull;
}

// --------------------------------------------------------------------------
// Walking the file
// --------------------------------------------------------------------------

/// One level-1 element inside the segment, or nullptr at the end.
EbmlElement* next_level1(MpDemux* d)
{
    return d->stream->FindNextElement(EBML_CONTEXT(d->segment.get()), d->upper,
                                      0xFFFFFFFFFFFFFFFFull, true, 1);
}

void read_track_entry(MpDemux* d, KaxTrackEntry& entry)
{
    Track t;
    if (auto* number = FindChild<KaxTrackNumber>(entry)) {
        t.number = static_cast<std::uint64_t>(*number);
    }
    if (auto* type = FindChild<KaxTrackType>(entry)) {
        t.kind = kind_for(static_cast<std::uint64_t>(*type));
    }
    if (auto* id = FindChild<KaxCodecID>(entry)) {
        t.codec_id = std::string(*id);
        t.codec = codec_for(t.codec_id);
    }
    if (auto* flag = FindChild<KaxTrackFlagDefault>(entry)) {
        t.is_default = static_cast<std::uint64_t>(*flag) != 0;
    }
    if (auto* delay = FindChild<KaxCodecDelay>(entry)) {
        // **Except for Vorbis, whose decoder has already applied it.**
        //
        // `CodecDelay` is meant to be a property of the codec, so any decoder
        // has it and any demuxer must state it. Vorbis is where that stops being
        // true: the format has no delay of its own -- Ogg trims the head with a
        // granule position and nothing else -- and libvorbis returns nothing at
        // all for the first packet, because a window with nothing to lap against
        // produces no samples. So what it hands back already begins at zero.
        //
        // ffmpeg writes `CodecDelay` here anyway, describing its own decoder,
        // which does emit those samples. Measured on the same two seconds of
        // audio: libvorbis returns 88320 frames, the file states 128 frames of
        // delay and 120 of padding, and 88320 - 120 is the 88200 the source WAV
        // had. Subtracting the 128 as well cuts 128 frames of real audio off the
        // front. The same file read as Ogg comes out at 88200 with no delay
        // applied, which is the other half of the same measurement.
        if (t.codec != MP_CODEC_VORBIS) {
            t.codec_delay_ns = static_cast<std::uint64_t>(*delay);
        }
    }

    std::uint32_t bits = 0;
    if (auto* audio = FindChild<KaxTrackAudio>(entry)) {
        if (auto* rate = FindChild<KaxAudioSamplingFreq>(*audio)) {
            t.format.sample_rate = static_cast<std::uint32_t>(static_cast<double>(*rate));
        }
        if (auto* channels = FindChild<KaxAudioChannels>(*audio)) {
            t.format.channels = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(*channels));
        }
        if (auto* depth = FindChild<KaxAudioBitDepth>(*audio)) {
            bits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(*depth));
        }
    }
    t.format.encoding = MP_ENCODING_PCM;
    t.format.channel_mask = 0;
    // **The sample type is the codec's**, with one exception: uncompressed
    // audio has no codec to ask, so what Matroska's BitDepth says is the whole
    // of it -- the same shape `demux_wav` has, for the same reason.
    t.format.sample_type = MP_SAMPLE_NONE;
    if (t.codec == MP_CODEC_PCM) {
        const std::uint32_t container = container_for(bits);
        t.format.valid_bits = bits;
        t.format.sample_type = sample_type_for(container, bits);
    } else if (bits != 0) {
        t.format.valid_bits = bits;
    }

    if (auto* priv = FindChild<KaxCodecPrivate>(entry)) {
        const auto* p = static_cast<const std::uint8_t*>(priv->GetBuffer());
        const auto n = static_cast<std::size_t>(priv->GetSize());
        switch (t.codec) {
        case MP_CODEC_VORBIS:
            if (!vorbis_config(p, n, t.config)) {
                log_fmt(MP_LOG_WARN, "track %llu: the Vorbis headers would not unpack",
                        static_cast<unsigned long long>(t.number));
                t.codec = MP_CODEC_UNKNOWN;
            }
            break;
        case MP_CODEC_FLAC:
            if (!flac_config(p, n, t.config)) {
                log_fmt(MP_LOG_WARN, "track %llu: no STREAMINFO in the FLAC CodecPrivate",
                        static_cast<unsigned long long>(t.number));
                t.codec = MP_CODEC_UNKNOWN;
            }
            break;
        default:
            // ALAC's magic cookie, AAC's AudioSpecificConfig and Opus's
            // OpusHead are all stored verbatim, which is what the ABI already
            // defines them as. Nothing to translate.
            t.config.assign(p, p + n);
            break;
        }
    }
    d->tracks.push_back(std::move(t));
}

/// MPEG audio's four-byte frame header, taken from the track's first packet.
///
/// **Matroska stores no CodecPrivate for A_MPEG/L1, L2 or L3**, because MPEG
/// audio has no setup data -- every frame restates the rate and the channel
/// mode in its own header. `codec_mpa` is handed those four bytes so it can
/// answer `get_format` before decoding anything, which is what the ABI asks of
/// it, so the container has to go and find them. Reading one packet is what a
/// demuxer is allowed to do in `open`.
bool peek_mpeg_header(MpDemux* d, std::size_t track, std::vector<std::uint8_t>& out);

/// Walks the segment once, collecting what a reader needs and skipping the
/// audio.
///
/// **It does not stop at the first cluster.** Tracks usually come first, but a
/// file written by a streaming muxer can put them anywhere, and a Cues element
/// is almost always at the end. Walking to the end costs one header read and one
/// seek per cluster, which for an hour of audio is a few thousand of each.
bool read_head(MpDemux* d)
{
    std::unique_ptr<EbmlElement> el{next_level1(d)};
    while (el) {
        const EbmlId id = EbmlId(*el);
        if (id == EBML_ID(KaxInfo)) {
            // **The level counter passed to `Read` must be a fresh one.**
            // libebml uses it as scratch while it descends, and handing it the
            // walk's own counter left this element read as an empty master --
            // silently, so the file simply had no duration and no timestamp
            // scale. Every `Read` here gets its own.
            int level = 0;
            EbmlElement* dummy = nullptr;
            el->Read(*d->stream, EBML_CONTEXT(el.get()), level, dummy, true);
            auto& info = static_cast<EbmlMaster&>(*el);
            if (auto* scale = FindChild<KaxTimestampScale>(info)) {
                const auto value = static_cast<std::uint64_t>(*scale);
                if (value != 0) {
                    d->timestamp_scale = value;
                }
            }
            if (auto* duration = FindChild<KaxDuration>(info)) {
                d->duration_scaled = static_cast<double>(*duration);
            }
        } else if (id == EBML_ID(KaxTracks)) {
            int level = 0;
            EbmlElement* dummy = nullptr;
            el->Read(*d->stream, EBML_CONTEXT(el.get()), level, dummy, true);
            auto& tracks = static_cast<EbmlMaster&>(*el);
            // `EbmlMaster::operator[]` indexes with an `unsigned`, and
            // `ListSize` answers in `size_t`. The loop counts in what the
            // subscript takes.
            for (unsigned i = 0; i < tracks.ListSize(); ++i) {
                if (EbmlId(*tracks[i]) == EBML_ID(KaxTrackEntry)) {
                    read_track_entry(d, *static_cast<KaxTrackEntry*>(tracks[i]));
                }
            }
        } else if (id == EBML_ID(KaxCues)) {
            // **Matroska's own seek index**, and the reason seeking here is a
            // lookup rather than a walk. It is almost always at the end of the
            // file, which is why this walk does not stop at the first cluster.
            int level = 0;
            EbmlElement* dummy = nullptr;
            el->Read(*d->stream, EBML_CONTEXT(el.get()), level, dummy, true);
            auto& cues = static_cast<EbmlMaster&>(*el);
            for (unsigned i = 0; i < cues.ListSize(); ++i) {
                if (EbmlId(*cues[i]) != EBML_ID(KaxCuePoint)) {
                    continue;
                }
                auto& point = static_cast<EbmlMaster&>(*cues[i]);
                auto* when = FindChild<KaxCueTime>(point);
                auto* where = FindChild<KaxCueTrackPositions>(point);
                if (when == nullptr || where == nullptr) {
                    continue;
                }
                auto* at = FindChild<KaxCueClusterPosition>(*where);
                if (at == nullptr) {
                    continue;
                }
                d->cues.push_back(MpDemux::Cue{
                    static_cast<std::uint64_t>(*when) * d->timestamp_scale,
                    static_cast<std::uint64_t>(*at)});
            }
        } else if (id == EBML_ID(KaxCluster)) {
            if (d->first_cluster == 0) {
                d->first_cluster = el->GetElementPosition();
            }
            el->SkipData(*d->stream, EBML_CONTEXT(el.get()));
        } else {
            el->SkipData(*d->stream, EBML_CONTEXT(el.get()));
        }
        el.reset(next_level1(d));
    }
    // Cue positions are relative to the segment's data, and the index here is
    // absolute, so they are made absolute once. A cue at the start of the audio
    // is added when the file did not write one, so the list is never empty.
    for (MpDemux::Cue& cue : d->cues) {
        cue.at += d->segment_data;
    }

    std::stable_sort(d->cues.begin(), d->cues.end(),
                     [](const MpDemux::Cue& a, const MpDemux::Cue& b) {
                         return a.ns < b.ns;
                     });
    if (d->cues.empty() || d->cues.front().ns != 0) {
        d->cues.insert(d->cues.begin(), MpDemux::Cue{0, d->first_cluster});
    }
    return !d->tracks.empty() && d->first_cluster != 0;
}

/// Positions the reader at `at`, which must be the start of a cluster.
bool restart_at(MpDemux* d, std::uint64_t at)
{
    d->block = nullptr;
    d->block_owner.reset();
    d->cluster.reset();
    d->pending.reset();
    d->next_lace = 0;
    d->ended = false;
    d->upper = 0;
    d->io->setFilePointer(static_cast<std::int64_t>(at), seek_beginning);
    return true;
}

/// The next block of the selected track, laced frames and all. False at the end.
bool next_block(MpDemux* d)
{
    for (int guard = 0; guard < (1 << 24); ++guard) {
        if (!d->cluster) {
            std::unique_ptr<EbmlElement> el{d->pending ? d->pending.release()
                                                       : next_level1(d)};
            while (el && EbmlId(*el) != EBML_ID(KaxCluster)) {
                el->SkipData(*d->stream, EBML_CONTEXT(el.get()));
                el.reset(next_level1(d));
            }
            if (!el) {
                return false;
            }
            d->cluster = std::move(el);
            d->cluster_ts = 0;
            d->upper = 0;
        }

        std::unique_ptr<EbmlElement> child{d->stream->FindNextElement(
            EBML_CONTEXT(d->cluster.get()), d->upper, 0xFFFFFFFFFFFFFFFFull, true, 1)};
        if (!child) {
            d->cluster.reset();
            return false;
        }
        if (d->upper > 0) {
            // **The cluster ended, and this element is what ended it.** libebml
            // has read its header already, so it is kept for the level-1 loop
            // rather than skipped -- skipping it loses whatever it contains,
            // which for the usual case of one cluster following another is a
            // second of audio.
            d->upper = 0;
            d->cluster.reset();
            d->pending = std::move(child);
            continue;
        }
        // A cluster of known size ends where it said it would.
        if (d->cluster->IsFiniteSize() &&
            child->GetElementPosition() >= d->cluster->GetEndPosition()) {
            d->cluster.reset();
            d->pending = std::move(child);
            continue;
        }

        const EbmlId id = EbmlId(*child);
        if (id == EBML_ID(KaxClusterTimestamp)) {
            int level = 0;
            EbmlElement* dummy = nullptr;
            child->Read(*d->stream, EBML_CONTEXT(child.get()), level, dummy, true);
            d->cluster_ts = static_cast<std::uint64_t>(
                *static_cast<KaxClusterTimestamp*>(child.get()));
            // **libmatroska needs telling.** A block stores a signed 16-bit
            // offset from its cluster's timestamp and nothing else, and
            // `GlobalTimestamp` resolves that against what the cluster was
            // initialised with. Upstream calls this "a dirty hack to get the
            // mandatory data back after reading", and it is the documented way.
            static_cast<KaxCluster*>(d->cluster.get())
                ->InitTimestamp(d->cluster_ts, static_cast<std::int64_t>(d->timestamp_scale));
            continue;
        }
        // **A block arrives two ways and ffmpeg uses both in one file.** Most
        // are SimpleBlocks; the last one of a track is usually a BlockGroup,
        // because that is the only form with a place to put the BlockDuration a
        // short final frame needs. Reading only the first kind loses exactly one
        // packet per file, which is how this was found.
        if (id == EBML_ID(KaxSimpleBlock) || id == EBML_ID(KaxBlockGroup)) {
            int inner_upper = 0;
            EbmlElement* dummy = nullptr;
            child->Read(*d->stream, EBML_CONTEXT(child.get()), inner_upper, dummy, true);

            KaxInternalBlock* block = nullptr;
            if (id == EBML_ID(KaxSimpleBlock)) {
                block = static_cast<KaxSimpleBlock*>(child.get());
            } else {
                block = FindChild<KaxBlock>(static_cast<EbmlMaster&>(*child));
            }
            if (block == nullptr || block->TrackNum() != d->track().number) {
                continue;
            }
            // **The cluster's timestamp is the block's base.** A block stores a
            // signed 16-bit offset from it and nothing else, so a block read
            // without its cluster has no position at all.
            block->SetParent(*static_cast<KaxCluster*>(d->cluster.get()));

            // **Two elements say where the audio really ends, and both live
            // in a BlockGroup and nowhere else.** That is why a muxer writes
            // the last block of a track as a group rather than a SimpleBlock:
            // an Opus stream ends on a whole 20 ms frame and a Vorbis stream on
            // a whole block, and a file needs somewhere to say which part of
            // that was real.
            //
            //  * `BlockDuration` is how long the block lasts. The spec is
            //    explicit that padding does *not* shorten it.
            //  * `DiscardPadding` is how much of the decoded output at the end
            //    of the block is that padding.
            //
            // So the audible end is the block's timestamp plus the first minus
            // the second. Taking only the first leaves the padding in, which is
            // eight milliseconds of an Opus file -- measured, before this was
            // read.
            d->block_duration_ns = 0;
            d->discard_ns = 0;
            if (id == EBML_ID(KaxBlockGroup)) {
                auto& group = static_cast<EbmlMaster&>(*child);
                if (auto* how_long = FindChild<KaxBlockDuration>(group)) {
                    d->block_duration_ns =
                        static_cast<std::uint64_t>(*how_long) * d->timestamp_scale;
                }
                if (auto* discard = FindChild<KaxDiscardPadding>(group)) {
                    // Signed, because it may be negative -- a gap rather than
                    // padding. A negative one asks for silence to be inserted,
                    // which nothing here does, so it is treated as none.
                    const std::int64_t value = static_cast<std::int64_t>(*discard);
                    d->discard_ns = value > 0 ? static_cast<std::uint64_t>(value) : 0;
                }
            }

            d->block_owner = std::move(child);
            d->block = block;
            d->next_lace = 0;
            return true;
        }
        child->SkipData(*d->stream, EBML_CONTEXT(child.get()));
    }
    return false;
}

/// What the selected track's last block says about where the audio stops.
///
/// **The segment's `Duration` is not that, and neither is either of these on its
/// own.** Duration is the length of the file, which for a lossy track is the
/// encoded length: an Opus stream ends on a whole 20 ms frame whatever the audio
/// did. The last block says more, in two elements that live in a BlockGroup and
/// nowhere else -- which is why a muxer writes the final block of a track as a
/// group rather than a SimpleBlock:
///
///  * `BlockDuration`, how long the block lasts, which gives an end timestamp.
///  * `DiscardPadding`, how much of the *decoded output* at the end of it is
///    the encoder's padding.
///
/// They answer different questions and only the second is exact. Measured on an
/// Opus file whose last block is at 2001 ms with a 7 ms duration and 13.5 ms of
/// padding: the timestamps are scaled to milliseconds, so an end taken from them
/// lands 72 frames from the truth however the two are combined, while 13.5 ms of
/// padding is 648 frames of 48 kHz exactly and 96960 - 312 - 648 is the 96000
/// frames FFmpeg produces, to the sample. So the end timestamp becomes
/// `total_frames`, which is a duration to display, and the padding becomes
/// `trim_frames`, which is arithmetic.
///
/// Costs one walk from the last cue to the end of the file, which is a cluster
/// or two -- not a pass over the whole thing.
Tail find_tail(MpDemux* d)
{
    const std::uint64_t from = d->cues.empty() ? d->first_cluster : d->cues.back().at;
    if (!restart_at(d, from)) {
        return {};
    }
    Tail tail{};
    for (int guard = 0; guard < (1 << 20); ++guard) {
        if (!next_block(d)) {
            break;
        }
        if (d->block_duration_ns != 0) {
            tail.end_ns = d->block->GlobalTimestamp() + d->block_duration_ns;
        } else {
            // A block with no stated duration after one that had it means the
            // stated one was not the last: forget it rather than trim early.
            tail.end_ns = 0;
        }
        // The padding, unlike the duration, is not confined to the last block --
        // a gapless join states it mid-file too. Only the final one is a tail.
        tail.trim_ns = d->discard_ns;
    }
    return tail;
}

/// The tail for `track`, worked out once and remembered.
const Tail& tail_of(MpDemux* d, std::size_t track)
{
    if (!d->tracks[track].tail_looked_for) {
        const std::size_t was = d->selected;
        d->selected = track;
        d->tracks[track].tail = find_tail(d);
        d->tracks[track].tail_looked_for = true;
        d->selected = was;
    }
    return d->tracks[track].tail;
}

bool peek_mpeg_header(MpDemux* d, std::size_t track, std::vector<std::uint8_t>& out)
{
    const std::size_t was = d->selected;
    d->selected = track;
    const bool got = next_block(d) && d->block != nullptr &&
                     d->block->NumberFrames() != 0 &&
                     d->block->GetBuffer(0).Size() >= 4;
    if (got) {
        const auto* p = static_cast<const std::uint8_t*>(d->block->GetBuffer(0).Buffer());
        out.assign(p, p + 4);
    }
    d->selected = was;
    return got;
}

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head, std::size_t bytes,
                             std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (head == nullptr || bytes < 4) {
        return MP_OK;
    }
    // EBML's own magic, which every Matroska and every WebM begins with. What
    // DocType it declares is inside the header rather than in the first word,
    // and reading it is `open`'s business -- a probe claims on what four bytes
    // prove, and these four prove EBML.
    if (head[0] == 0x1Au && head[1] == 0x45u && head[2] == 0xDFu && head[3] == 0xA3u) {
        *out_score = 100;
    }
    return MP_OK;
}

MpResult MP_CALL demux_open(const char* path, MpDemux** out) noexcept
try {
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    auto d = std::make_unique<MpDemux>();
    try {
        d->io = std::make_unique<StdIOCallback>(path, MODE_READ);
    } catch (...) {
        return MP_ERR_IO;
    }
    d->stream = std::make_unique<EbmlStream>(*d->io);

    std::unique_ptr<EbmlElement> head{d->stream->FindNextID(EBML_INFO(EbmlHead), 0xFFFFFFFFull)};
    if (!head) {
        return MP_ERR_UNSUPPORTED;
    }
    {
        int level = 0;
        EbmlElement* dummy = nullptr;
        head->Read(*d->stream, EBML_CONTEXT(head.get()), level, dummy, true);
        auto& master = static_cast<EbmlMaster&>(*head);
        if (auto* type = FindChild<EDocType>(master)) {
            const std::string doc(*type);
            if (doc != "matroska" && doc != "webm") {
                log_fmt(MP_LOG_DEBUG, "%s is EBML but its DocType is `%s`", path,
                        doc.c_str());
                return MP_ERR_UNSUPPORTED;
            }
        }
    }

    d->segment.reset(d->stream->FindNextID(EBML_INFO(KaxSegment), 0xFFFFFFFFFFFFFFFFull));
    if (!d->segment) {
        return MP_ERR_UNSUPPORTED;
    }
    // Cue positions count from the segment's data, and libmatroska is what
    // knows where that is -- `GetGlobalPosition` is the segment turning one
    // into the other.
    d->segment_data =
        static_cast<KaxSegment*>(d->segment.get())->GetGlobalPosition(0);
    if (!read_head(d.get())) {
        log_fmt(MP_LOG_DEBUG, "%s: no tracks or no clusters this demuxer could find", path);
        return MP_ERR_UNSUPPORTED;
    }

    // The default audio track, or the first audio track, or nothing. **A file is
    // not one stream**, and picking is a decision rather than an accident.
    std::size_t chosen = d->tracks.size();
    for (std::size_t i = 0; i < d->tracks.size(); ++i) {
        if (d->tracks[i].kind != MP_STREAM_AUDIO) {
            continue;
        }
        if (chosen == d->tracks.size() || d->tracks[i].is_default) {
            chosen = i;
            if (d->tracks[i].is_default) {
                break;
            }
        }
    }
    if (chosen == d->tracks.size()) {
        log_fmt(MP_LOG_DEBUG, "%s carries no audio track", path);
        return MP_ERR_UNSUPPORTED;
    }
    d->selected = chosen;

    if (!restart_at(d.get(), d->first_cluster)) {
        return MP_ERR_IO;
    }

    Track& chosen_track = d->tracks[chosen];
    if (chosen_track.config.empty() &&
        (chosen_track.codec == MP_CODEC_MP1 || chosen_track.codec == MP_CODEC_MP2 ||
         chosen_track.codec == MP_CODEC_MP3)) {
        if (!peek_mpeg_header(d.get(), chosen, chosen_track.config) ||
            !restart_at(d.get(), d->first_cluster)) {
            log_fmt(MP_LOG_WARN, "%s: could not read an MPEG frame header from the "
                                 "first packet",
                    path);
            return MP_ERR_UNSUPPORTED;
        }
    }

    *out = d.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_stream_count(MpDemux* d, std::uint32_t* out_count) noexcept
{
    if (d == nullptr || out_count == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_count = static_cast<std::uint32_t>(d->tracks.size());
    return MP_OK;
}

MpResult MP_CALL demux_stream_info(MpDemux* d, std::uint32_t index, MpStreamInfo* out) noexcept
try {
    if (d == nullptr || out == nullptr || index >= d->tracks.size()) {
        return MP_ERR_INVALID;
    }
    const Track& t = d->tracks[index];
    const std::uint32_t size = out->size;
    std::memset(out, 0, size);
    out->size = size;
    out->index = index;
    out->kind = t.kind;
    out->codec = t.codec;
    out->flags = index == d->selected ? MP_STREAM_DEFAULT : 0u;
    out->config_bytes = static_cast<std::uint32_t>(t.config.size());
    out->format = t.format;

    // **The gapless edit, both ends of it.**
    //
    // The head is Matroska's CodecDelay, stated in nanoseconds rather than in
    // samples -- the same fact `elst` carries in MP4 and `pre_skip` carries in
    // an Opus header, spelled a third way, which is exactly why MpStreamInfo
    // carries it and a codec never sees it.
    //
    // The tail is `DiscardPadding`, and it goes in `trim_frames` rather than in
    // `play_frames` -- **`play_frames` stays 0, because Matroska cannot state
    // it.** Every timestamp in the file is scaled to the millisecond, so a
    // length derived from one is rounded, and a rounded length applied to a
    // lossless track truncates it. The padding is exact whatever the scale, so
    // that is the number worth having. `find_tail` has the measurement.
    if (t.format.sample_rate != 0) {
        if (t.codec_delay_ns != 0) {
            out->skip_frames = to_frames(t.codec_delay_ns, t.format.sample_rate);
        }
        const Tail& tail = tail_of(d, index);
        out->trim_frames = to_frames(tail.trim_ns, t.format.sample_rate);

        // A duration for the display, from the last block's own end where it
        // gave one and the segment's Duration where it did not. Approximate by
        // a millisecond, which is what it is for, and never used as a bound.
        std::uint64_t ns = tail.end_ns;
        if (ns == 0 && d->duration_scaled > 0.0) {
            ns = static_cast<std::uint64_t>(d->duration_scaled *
                                            static_cast<double>(d->timestamp_scale));
        }
        if (ns != 0) {
            const std::uint64_t frames = to_frames(ns, t.format.sample_rate);
            const std::uint64_t gone = out->skip_frames + out->trim_frames;
            out->total_frames = frames > gone ? frames - gone : 0;
        }
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index >= d->tracks.size() || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto& config = d->tracks[index].config;
    const auto needed = static_cast<std::uint32_t>(config.size());
    *out_needed = needed;
    if (out == nullptr) {
        return MP_OK;
    }
    if (out_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }
    if (needed != 0) {
        std::memcpy(out, config.data(), needed);
    }
    return MP_OK;
}

MpResult MP_CALL demux_select_streams(MpDemux* d, const std::uint32_t* indices,
                                      std::uint32_t count) noexcept
try {
    if (d == nullptr || indices == nullptr || count == 0) {
        return MP_ERR_INVALID;
    }
    // **One at a time, for now, and that is a statement about this module
    // rather than about Matroska.** The container interleaves at cluster
    // granularity and could serve several; what is single here is the reader --
    // the lace cursor, `position` and `frames_of` are all the selected track's.
    // Making them per-track is the change this module needs the day a video
    // path reads a Matroska, and `demux_mp4` is where v3's several-streams is
    // proved until then.
    if (count > 1) {
        return MP_ERR_UNSUPPORTED;
    }
    if (indices[0] >= d->tracks.size()) {
        return MP_ERR_INVALID;
    }
    d->selected = indices[0];
    d->position = 0;
    return restart_at(d, d->first_cluster) ? MP_OK : MP_ERR_IO;
} catch (...) {
    return MP_ERR_IO;
}

MpResult MP_CALL demux_read_packet(MpDemux* d, void* dst, std::size_t dst_bytes,
                                   MpPacket* out) noexcept
try {
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, size);
    out->size = size;
    if (d->ended) {
        return MP_END;
    }

    while (d->block == nullptr || d->next_lace >= d->block->NumberFrames()) {
        d->block = nullptr;
        d->block_owner.reset();
        if (!next_block(d)) {
            d->ended = true;
            return MP_END;
        }
    }

    DataBuffer& lace = d->block->GetBuffer(d->next_lace);
    const auto length = static_cast<std::size_t>(lace.Size());
    if (length == 0) {
        ++d->next_lace;
        return MP_ERR_FORMAT;
    }
    if (dst == nullptr || dst_bytes < length) {
        // Nothing is consumed: the lace stays where it is and the next call
        // hands back the same one.
        out->bytes = static_cast<std::uint32_t>(length);
        return MP_ERR_NO_MEMORY;
    }
    std::memcpy(dst, lace.Buffer(), length);
    out->bytes = static_cast<std::uint32_t>(length);

    // **Matroska timestamps blocks, not laces**, so only the first frame of a
    // laced block has a position the container stated. The rest are between two
    // known points, and saying so is what MP_PACKET_TIMED is for -- a demuxer
    // that guessed would put every seek in a laced file wrong by up to a lace.
    if (d->next_lace == 0) {
        const std::uint64_t ns = d->block->GlobalTimestamp();
        d->position = frames_of(d, ns);
        out->frame = d->position;
        out->flags = MP_PACKET_TIMED;
        out->stream = static_cast<std::uint32_t>(d->selected);
    } else {
        out->frame = 0;
        out->flags = 0;
        out->stream = static_cast<std::uint32_t>(d->selected);
    }
    ++d->next_lace;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint32_t stream,
                            std::uint64_t frame) noexcept
try {
    if (d == nullptr || stream >= d->tracks.size()) {
        return MP_ERR_INVALID;
    }
    // **To the cluster, and the host does the rest.** Matroska's Cues map a
    // timestamp to the position of the cluster holding it, which is a cluster
    // boundary rather than a sample -- so this lands *before* the target and
    // never after it, and `MP_PACKET_TIMED` on the first block is what lets the
    // host discard the difference. Landing after the target would be
    // unrecoverable; landing before it costs one cluster of decoding.
    // `frame` is in the named stream's own rate, which is not necessarily the
    // selected one's -- a host seeking by the audio clock names the audio
    // stream whether or not it is the stream it is reading.
    const std::uint64_t rate = d->tracks[stream].format.sample_rate;
    if (rate == 0) {
        return MP_ERR_UNSUPPORTED;
    }
    const std::uint64_t target_ns =
        frame / rate * 1000000000ull + frame % rate * 1000000000ull / rate;

    MpDemux::Cue best = d->cues.front();
    for (const MpDemux::Cue& cue : d->cues) {
        if (cue.ns <= target_ns && cue.ns >= best.ns) {
            best = cue;
        }
    }
    if (!restart_at(d, best.at)) {
        return MP_ERR_IO;
    }
    d->position = frames_of(d, best.ns);
    return MP_OK;
} catch (...) {
    return MP_ERR_IO;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
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

const MpDemuxVtbl g_vtbl = {
    /* size          */ sizeof(MpDemuxVtbl),
    /* reserved      */ 0,
    /* probe         */ &demux_probe,
    /* open          */ &demux_open,
    /* stream_count  */ &demux_stream_count,
    /* stream_info   */ &demux_stream_info,
    /* stream_config */ &demux_stream_config,
    /* select_streams*/ &demux_select_streams,
    /* read_packet   */ &demux_read_packet,
    /* seek          */ &demux_seek,
    /* read_frames   */ nullptr, // it splits properly; there is nothing to decode
    /* close         */ &demux_close,
    /* stream_video_info */ nullptr, // not until this module reads two streams
};

/// Everything this module can name. A codec on the list that no module here
/// decodes is still worth naming: "an MKV carrying DTS" is a better answer than
/// "an MKV I cannot read", and it is what sends the file to FFmpeg with a
/// reason attached.
const MpCodec g_codecs[] = {MP_CODEC_FLAC,   MP_CODEC_VORBIS, MP_CODEC_OPUS,
                            MP_CODEC_AAC_LC, MP_CODEC_MP1,    MP_CODEC_MP2,
                            MP_CODEC_MP3,    MP_CODEC_ALAC,   MP_CODEC_PCM,
                            MP_CODEC_AC3,    MP_CODEC_EAC3,   MP_CODEC_DTS,
                            MP_CODEC_WAVPACK, MP_CODEC_TTA};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 110,
    /* id          */ "demux_mkv",
    /* name        */ "Matroska and WebM (libmatroska, the reference container)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ static_cast<std::uint32_t>(sizeof(g_codecs) / sizeof(g_codecs[0])),
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

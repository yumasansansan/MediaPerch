// SPDX-License-Identifier: GPL-3.0-or-later
//
// MP4, as a container and nothing else, on Bento4.
//
// **This module used to be a parser written here, and the reason it is not any
// more is the reason recorded for FLAC and Matroska.** Where a reference
// implementation of the container exists, this tree reads the container with it
// and keeps its own code for the places where none does. The five hundred lines
// that went away read twelve boxes of ISO 14496-12 -- `moov`, `trak`, `mdia`,
// `minf`, `stbl`, `stsd`, `stsz`, `stco`/`co64`, `stsc`, `stts`, `mdhd`, `mvhd`
// and `elst` -- which is the shape a music file has and nothing else. Measured
// at the time: a fragmented MP4, a DASH segment and a QuickTime `.mov` were all
// declined, and FFmpeg read them instead.
//
// `ap4` reads the standard. What that buys, measured the same way:
//
//   * **Fragmented MP4.** `moof`/`traf`/`trun`, which is how every DASH, CMAF
//     and HLS-fMP4 stream is written, and how ffmpeg writes an MP4 that can be
//     produced without seeking backwards. There is no `stbl` in such a file at
//     all, so the old parser had nothing to read.
//   * **QuickTime `.mov`**, which is the same box structure under a different
//     brand and was declined for no better reason than that.
//   * **Every track, not the first audio one.** `MpStreamInfo` has always been
//     able to describe a file as several streams; this module could not, and
//     now says what is in the file the way `demux_mkv` does.
//
// It still decodes nothing. `AP4_LinearReader` hands over samples; which codec
// gets them is a table lookup on what `stsd` said, and the configuration blob
// goes across verbatim.

#include "mp4_guard.hpp"

#include <mediaperch/module.h>

#include <Ap4.h>
#include <Ap4ColrAtom.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    if (g_host == nullptr || g_host->log == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_host->log(g_host->ctx, level, line);
}

/// Stream operations one `open` may spend, and one packet or seek may spend.
///
/// Parsing is proportional to a file's *boxes*, not its samples: a normal MP4
/// has dozens, and even a pathological but legal one with a hundred thousand
/// stays far under a million. Reading a packet is a seek and a read; a
/// fragmented one adds a walk over the boxes of a single fragment.
constexpr std::uint64_t k_parse_budget = 1000000;
constexpr std::uint64_t k_packet_budget = 100000;

FILE* open_utf8(const char* path) noexcept
{
#if defined(_WIN32)
    const int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (len <= 0) {
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), len);
    FILE* fp = nullptr;
    return _wfopen_s(&fp, wide.c_str(), L"rb") == 0 ? fp : nullptr;
#else
    return std::fopen(path, "rb");
#endif
}

/// A read-only AP4_ByteStream over a FILE* this module opened itself, with a
/// budget.
///
/// **Not AP4_FileByteStream**, for two reasons.
///
/// The first is that it opens with the narrow CRT call: on Windows that is the
/// process code page, so a path with a character outside it -- most of a
/// Japanese music library -- would not open at all. Every other module here goes
/// through the same `open_utf8`, and this is the adapter that lets Bento4 do the
/// same.
///
/// **The second is the budget, and it is a security control.** Bento4 has more
/// than one box parser that reads a count out of a file and then loops that many
/// times without checking the count against the bytes the box actually has.
/// `mp4_fuzzer` found two in the first ten minutes it ran:
///
///  * `sgpd` -- 67,108,865 entries declared by a 26-byte box, one allocation
///    each. Measured: 2 GB and no return, from a 1143-byte file.
///  * `dref` -- 956,301,312 entries declared by a 28-byte box. The inner loop
///    drains the stream on the first pass, so the other 956 million iterations
///    each do a `Tell`, two `ReadUI32`s and a `Seek` against nothing. Measured:
///    84 seconds, from a 1269-byte file.
///
/// Both are the same defect, and finding two of them in ten minutes is a good
/// reason to assume more. Suppressing boxes one at a time is whack-a-mole; the
/// stream is the one thing every parser in the library has to come through, so
/// the bound goes here. Each entry point arms a budget, and past it every read
/// and seek reports end-of-stream -- which is a thing every parser in Bento4
/// already handles, because a truncated file does the same.
///
/// The number is deliberately far above any real file: parsing is proportional
/// to the *boxes* in a file, not its samples, and even a pathological but legal
/// MP4 with a hundred thousand boxes stays under a million operations. Reading
/// one packet costs a seek and a read.
class FileStream : public AP4_ByteStream {
public:
    FileStream(FILE* fp, AP4_LargeSize size) noexcept : fp_(fp), size_(size) {}

    /// Starts a new budget. Called once per module entry point, so that a long
    /// playback is not bounded -- one *call* is.
    void arm(std::uint64_t operations) noexcept
    {
        used_ = 0;
        budget_ = operations;
    }

    /// Whether the last armed budget ran out, so a caller can say why rather
    /// than reporting a truncated file.
    [[nodiscard]] bool exhausted() const noexcept { return used_ > budget_; }

    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    // Bento4 hands streams around by reference count rather than by owner.
    void AddReference() override { ++refs_; }
    void Release() override
    {
        if (--refs_ == 0) {
            delete this;
        }
    }

    AP4_Result ReadPartial(void* buffer, AP4_Size want, AP4_Size& got) override
    {
        got = 0;
        if (++used_ > budget_) {
            return AP4_ERROR_EOS;
        }
        got = static_cast<AP4_Size>(std::fread(buffer, 1, want, fp_));
        if (got == 0) {
            return want == 0 ? AP4_SUCCESS : AP4_ERROR_EOS;
        }
        return AP4_SUCCESS;
    }

    // Nothing here writes a file. Saying so is better than a stub that looks
    // like it might.
    AP4_Result WritePartial(const void*, AP4_Size, AP4_Size& written) override
    {
        written = 0;
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_Result Seek(AP4_Position position) override
    {
        if (++used_ > budget_) {
            return AP4_ERROR_EOS;
        }
        return _fseeki64(fp_, static_cast<std::int64_t>(position), SEEK_SET) == 0
                   ? AP4_SUCCESS
                   : AP4_ERROR_EOS;
    }

    AP4_Result Tell(AP4_Position& position) override
    {
        const std::int64_t at = _ftelli64(fp_);
        if (at < 0) {
            return AP4_FAILURE;
        }
        position = static_cast<AP4_Position>(at);
        return AP4_SUCCESS;
    }

    AP4_Result GetSize(AP4_LargeSize& size) override
    {
        size = size_;
        return AP4_SUCCESS;
    }

    /// Opens `path`, or returns null. The caller owns one reference.
    static FileStream* open(const char* path) noexcept
    {
        FILE* fp = open_utf8(path);
        if (fp == nullptr) {
            return nullptr;
        }
        std::int64_t bytes = -1;
        if (_fseeki64(fp, 0, SEEK_END) == 0) {
            bytes = _ftelli64(fp);
        }
        if (bytes < 0 || _fseeki64(fp, 0, SEEK_SET) != 0) {
            std::fclose(fp);
            return nullptr;
        }
        auto* stream = new (std::nothrow)
            FileStream(fp, static_cast<AP4_LargeSize>(bytes));
        if (stream == nullptr) {
            std::fclose(fp);
        }
        return stream;
    }

protected:
    // Only Release() may destroy one, which is what AP4_Referenceable means.
    ~FileStream() override
    {
        if (fp_ != nullptr) {
            std::fclose(fp_);
        }
    }

private:
    FILE* fp_ = nullptr;
    AP4_LargeSize size_ = 0;
    int refs_ = 1;
    std::uint64_t used_ = 0;
    std::uint64_t budget_ = k_parse_budget;
};

/// Frees a Bento4 object by releasing a reference rather than deleting it.
struct Releaser {
    template <typename T>
    void operator()(T* object) const noexcept
    {
        if (object != nullptr) {
            object->Release();
        }
    }
};

MpStreamKind kind_for(AP4_Track::Type type) noexcept
{
    switch (type) {
    case AP4_Track::TYPE_AUDIO:
        return MP_STREAM_AUDIO;
    case AP4_Track::TYPE_VIDEO:
        return MP_STREAM_VIDEO;
    case AP4_Track::TYPE_TEXT:
    case AP4_Track::TYPE_SUBTITLES:
        return MP_STREAM_SUBTITLE;
    default:
        return MP_STREAM_OTHER;
    }
}

/// The ALAC magic cookie, out of the `alac` box inside an `alac` sample entry.
///
/// Bento4 has no class for ALAC, so the box arrives as an AP4_UnknownAtom whose
/// payload it keeps private -- the way to read one is to ask it to write itself.
/// What comes back is the whole box: an 8-byte header, then the version and
/// flags every full box carries, then the 24 bytes of ALACSpecificConfig that
/// `codec_alac` is defined to take.
///
/// **Two places, because QuickTime puts it somewhere else.** An MP4 written by
/// ffmpeg has the `alac` box as a direct child of the sample entry; a `.mov`
/// wraps it in `wave`, QuickTime's own container for a decompression parameter
/// list, beside `frma` and `chan`. Measured on the same audio encoded both ways:
/// the first layout has one child, `alac`; the second has `wave` and `chan`, and
/// looking only in the first place is what made this module decline every `.mov`
/// with "nothing here decodes that codec".
bool alac_config(const AP4_SampleDescription& desc, std::vector<std::uint8_t>& out)
{
    auto& details = const_cast<AP4_AtomParent&>(desc.GetDetails());
    AP4_Atom* box = details.GetChild(AP4_ATOM_TYPE_ALAC);
    if (box == nullptr) {
        box = details.FindChild("wave/alac");
    }
    if (box == nullptr) {
        return false;
    }
    auto* bytes = new (std::nothrow) AP4_MemoryByteStream();
    if (bytes == nullptr) {
        return false;
    }
    const std::unique_ptr<AP4_ByteStream, Releaser> owner(bytes);
    if (AP4_FAILED(box->Write(*bytes))) {
        return false;
    }
    constexpr AP4_Size k_header = 8 + 4; // box header, then version and flags
    constexpr AP4_Size k_config = 24;    // ALACSpecificConfig
    if (bytes->GetDataSize() < k_header + k_config) {
        return false;
    }
    const AP4_UI08* at = bytes->GetData() + k_header;
    out.assign(at, at + k_config);
    return true;
}

/// What `stsd` said, mapped onto an MpCodec and the blob the ABI defines for it.
///
/// A demuxer that passed the fourcc straight through would make two containers
/// spelling one codec differently into two codecs, which is the whole reason
/// this mapping exists rather than the number travelling raw.
MpCodec codec_for(AP4_SampleDescription* desc, std::vector<std::uint8_t>& config)
{
    config.clear();
    if (desc == nullptr) {
        return MP_CODEC_UNKNOWN;
    }

    if (desc->GetFormat() == AP4_SAMPLE_FORMAT_ALAC) {
        return alac_config(*desc, config) ? MP_CODEC_ALAC : MP_CODEC_UNKNOWN;
    }

    if (auto* mpeg = AP4_DYNAMIC_CAST(AP4_MpegAudioSampleDescription, desc)) {
        // MPEG-4 audio and the three MPEG-2 AAC profiles all carry an
        // AudioSpecificConfig, which is what `codec_aac` is defined to take.
        // MPEG-1/2 layer audio in an MP4 does not, and is left unnamed rather
        // than handed over as something it is not.
        const AP4_UI08 oti = mpeg->GetObjectTypeId();
        const bool is_aac = oti == AP4_OTI_MPEG4_AUDIO ||
                            oti == AP4_OTI_MPEG2_AAC_AUDIO_MAIN ||
                            oti == AP4_OTI_MPEG2_AAC_AUDIO_LC ||
                            oti == AP4_OTI_MPEG2_AAC_AUDIO_SSRP;
        if (!is_aac) {
            return MP_CODEC_UNKNOWN;
        }
        const AP4_DataBuffer& info = mpeg->GetDecoderInfo();
        if (info.GetDataSize() == 0) {
            return MP_CODEC_UNKNOWN;
        }
        config.assign(info.GetData(), info.GetData() + info.GetDataSize());
        return MP_CODEC_AAC_LC;
    }

    // Read perfectly, and carrying something nothing here names. That is a
    // sentence a host can act on, unlike "this demuxer declines".
    return MP_CODEC_UNKNOWN;
}

/// One track, as this module reports it.
struct Stream {
    AP4_Track* track = nullptr; ///< owned by the movie, not by this
    MpStreamKind kind = MP_STREAM_OTHER;
    MpCodec codec = MP_CODEC_UNKNOWN;
    std::vector<std::uint8_t> config;
    MpFormat format{};
    std::uint64_t total_frames = 0;
    std::uint64_t skip_frames = 0;
    std::uint64_t play_frames = 0;
    bool is_default = false;
};

/// The gapless edit, from `edts`/`elst`.
///
/// **This is the box most demuxers skip**, and it is the whole of what separates
/// a decoder that starts a track twenty-one milliseconds late from one that does
/// not. `media_time` is where the audio begins, in media frames; the segment
/// duration is how much of it is real, in the *movie* timescale, which is not
/// always the same one.
void read_edit(Stream& s)
{
    auto* trak = const_cast<AP4_TrakAtom*>(s.track->GetTrakAtom());
    if (trak == nullptr) {
        return;
    }
    auto* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom, trak->FindChild("edts/elst"));
    if (elst == nullptr) {
        return;
    }
    AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
    for (AP4_Cardinal i = 0; i < entries.ItemCount(); ++i) {
        if (entries[i].m_MediaTime < 0) {
            continue; // an empty edit: silence, not a delay
        }
        s.skip_frames = static_cast<std::uint64_t>(entries[i].m_MediaTime);

        // The segment duration is stated in the movie timescale and the media
        // time in the track's own. They are usually equal for an audio-only
        // file and there is no reason to rely on that.
        const std::uint64_t duration = entries[i].m_SegmentDuration;
        const std::uint32_t movie = s.track->GetMovieTimeScale();
        const std::uint32_t media = s.track->GetMediaTimeScale();
        s.play_frames = (duration != 0 && movie != 0 && media != 0 && movie != media)
                            ? AP4_ConvertTime(duration, movie, media)
                            : duration;
        return; // the first real edit is the one that describes the audio
    }
}

} // namespace

struct MpDemux {
    std::unique_ptr<FileStream, Releaser> stream;
    std::unique_ptr<AP4_File> file;
    AP4_Movie* movie = nullptr; ///< owned by `file`
    std::vector<Stream> streams;
    /// The selected set, in `streams` order. **v3**: one index was enough while
    /// nothing read two streams at once; a player showing video reads both out
    /// of one file, and reading them by opening the container twice means two
    /// positions and two seeks that have to agree.
    std::vector<std::size_t> selected;

    /// Rebuilt on every seek, because it is a cursor and not a reader.
    std::unique_ptr<AP4_LinearReader> reader;
    AP4_DataBuffer sample_data;
    bool at_end = false;

    /// Held back when the host's buffer was too small, so the packet is not
    /// read from the file twice.
    bool have_pending = false;
    std::uint64_t pending_frame = 0;
    std::uint32_t pending_stream = 0;
};

namespace {

/// Points the reader at the start, and then at `index` within `in_track` when
/// one is named -- a seek names the stream it is seeking, and the others follow
/// from wherever the file position lands.
bool restart(MpDemux* d, AP4_Track* in_track, AP4_Ordinal index)
{
    d->reader.reset();
    d->at_end = false;
    d->have_pending = false;
    if (d->movie == nullptr || d->streams.empty() || d->selected.empty()) {
        return false;
    }

    // **Rewound first, because the reader starts looking for fragments wherever
    // the stream happens to be.** Its constructor takes the current position as
    // the first fragment's, and parsing the file left that at the end -- so a
    // fragmented MP4 opened cleanly, reported its track, and then produced zero
    // samples, because every `moof` was already behind the cursor. Measured: 0
    // frames against ffmpeg's 90112, on a file ffmpeg wrote with
    // `-movflags +frag_keyframe+empty_moov`.
    //
    // From zero it walks the whole box list and skips what is not a `moof`,
    // which costs one pass over the headers. A non-fragmented file never gets
    // there at all and takes its samples straight out of the table.
    d->stream->arm(k_parse_budget);
    if (AP4_FAILED(d->stream->Seek(0))) {
        return false;
    }
    auto reader = std::unique_ptr<AP4_LinearReader>(
        new (std::nothrow) AP4_LinearReader(*d->movie, d->stream.get()));
    if (reader == nullptr) {
        return false;
    }
    // Every selected track, because `ReadNextSample` walks them together in
    // storage order and that is the whole point of selecting more than one.
    for (const std::size_t at : d->selected) {
        AP4_Track* track = d->streams[at].track;
        if (track == nullptr || AP4_FAILED(reader->EnableTrack(track->GetId()))) {
            return false;
        }
    }
    if (in_track != nullptr && index != 0 &&
        AP4_FAILED(reader->SetSampleIndex(in_track->GetId(), index))) {
        return false;
    }
    d->reader = std::move(reader);
    return true;
}

/// The stream index a Bento4 track id belongs to, or `npos`.
std::size_t stream_of_track(const MpDemux* d, AP4_UI32 track_id)
{
    for (std::size_t at = 0; at < d->streams.size(); ++at) {
        const AP4_Track* track = d->streams[at].track;
        if (track != nullptr && track->GetId() == track_id) {
            return at;
        }
    }
    return static_cast<std::size_t>(-1);
}

MpResult MP_CALL demux_probe(const char* path, const std::uint8_t* head,
                             std::size_t bytes, std::uint32_t* out_score) noexcept
{
    (void)path;
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    // **A question about the container only.** What is inside is not a probe's
    // business -- `moov` may be at the end of a file a probe sees four kilobytes
    // of, and under v1 that meant two decoders each claiming every MP4 and then
    // finding out.
    //
    // `ftyp` is the first box of a conformant file. A QuickTime `.mov` may have
    // `moov` first instead and no `ftyp` at all, which is why that is a second,
    // weaker answer rather than no answer: it is the same box structure and
    // `ap4` reads it.
    if (head != nullptr && bytes >= 12) {
        if (std::memcmp(head + 4, "ftyp", 4) == 0) {
            *out_score = 100;
        } else if (std::memcmp(head + 4, "moov", 4) == 0 ||
                   std::memcmp(head + 4, "skip", 4) == 0 ||
                   std::memcmp(head + 4, "wide", 4) == 0) {
            *out_score = 70;
        }
    }
    return MP_OK;
}

MpResult MP_CALL demux_open(const char* path, MpDemux** out) noexcept
try {
    if (path == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    auto d = std::unique_ptr<MpDemux>(new (std::nothrow) MpDemux());
    if (d == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->stream.reset(FileStream::open(path));
    if (d->stream == nullptr) {
        return MP_ERR_IO;
    }

    // `moov_only` false: a fragmented file keeps its sample tables in the
    // `moof`s, and the reader needs the whole thing to walk them.
    //
    // The factory is ours because one box Bento4 parses can be made to allocate
    // without bound -- see mp4_guard.hpp for the file that does it. It is a
    // local because `AP4_File` parses in its constructor and does not keep it.
    mp::mp4::GuardedAtomFactory factory;
    d->stream->arm(k_parse_budget);
    d->file.reset(new (std::nothrow) AP4_File(*d->stream, factory, false));
    if (d->stream->exhausted()) {
        // Not a truncated file: a file that asked the parser to do more work
        // than any real one needs. Saying which is the difference between a
        // diagnosis and a shrug.
        log_fmt(MP_LOG_WARN, "%s: gave up parsing after %llu stream operations",
                path, static_cast<unsigned long long>(k_parse_budget));
        return MP_ERR_UNSUPPORTED;
    }
    if (d->file == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    d->movie = d->file->GetMovie();
    if (d->movie == nullptr) {
        log_fmt(MP_LOG_DEBUG, "%s: no moov -- not an MP4 this reads", path);
        return MP_ERR_UNSUPPORTED;
    }

    // **Every track, in the file's own order.** The old parser found one audio
    // track and reported a stream count of 1 whatever else was there.
    AP4_List<AP4_Track>& tracks = d->movie->GetTracks();
    for (AP4_List<AP4_Track>::Item* item = tracks.FirstItem(); item != nullptr;
         item = item->GetNext()) {
        AP4_Track* track = item->GetData();
        if (track == nullptr) {
            continue;
        }
        Stream s;
        s.track = track;
        s.kind = kind_for(track->GetType());
        s.codec = codec_for(track->GetSampleDescription(0), s.config);
        s.total_frames = track->GetMediaDuration();

        // What the *container* states about the audio. The codec's own
        // configuration decides the depth -- ALAC's states it and AAC's does
        // not -- so only the rate is taken here, and `PacketSource` lets the
        // codec's answer win where the two differ.
        if (s.kind == MP_STREAM_AUDIO) {
            s.format.sample_rate = track->GetMediaTimeScale();
            s.format.encoding = MP_ENCODING_PCM;
            read_edit(s);
        }
        d->streams.push_back(std::move(s));
    }
    if (d->streams.empty()) {
        log_fmt(MP_LOG_DEBUG, "%s: a moov with no tracks in it", path);
        return MP_ERR_UNSUPPORTED;
    }

    // The first audio track is the default, which is what a player wants and
    // what the old module reported as the only one.
    for (std::size_t i = 0; i < d->streams.size(); ++i) {
        if (d->streams[i].kind == MP_STREAM_AUDIO) {
            d->streams[i].is_default = true;
            d->selected = {i};
            break;
        }
    }
    if (d->selected.empty() && !d->streams.empty()) {
        d->selected = {0}; // a file with no audio at all still opens
    }
    if (!restart(d.get(), nullptr, 0)) {
        return MP_ERR_UNSUPPORTED;
    }

    *out = d.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_stream_count(MpDemux* d, std::uint32_t* out) noexcept
{
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = static_cast<std::uint32_t>(d->streams.size());
    return MP_OK;
}

MpResult MP_CALL demux_stream_info(MpDemux* d, std::uint32_t index,
                                   MpStreamInfo* out) noexcept
{
    if (d == nullptr || out == nullptr || index >= d->streams.size()) {
        return MP_ERR_INVALID;
    }
    const Stream& s = d->streams[index];
    out->index = index;
    out->kind = s.kind;
    out->codec = s.codec;
    out->flags = s.is_default ? MP_STREAM_DEFAULT : 0u;
    out->config_bytes = static_cast<std::uint32_t>(s.config.size());
    out->format = s.format;
    out->total_frames = s.total_frames;
    // **The gapless edit, which was always the container's.** `elst` says how
    // much of the front is the encoder's warm-up and how much of the rest is
    // the audio. v1 applied this inside each decoder; now it is stated once and
    // applied once.
    out->skip_frames = s.skip_frames;
    out->play_frames = s.play_frames;
    return MP_OK;
}

MpResult MP_CALL demux_stream_config(MpDemux* d, std::uint32_t index, std::uint8_t* out,
                                     std::uint32_t out_bytes,
                                     std::uint32_t* out_needed) noexcept
{
    if (d == nullptr || index >= d->streams.size() || out_needed == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto& config = d->streams[index].config;
    const auto needed = static_cast<std::uint32_t>(config.size());
    *out_needed = needed;
    if (out == nullptr) {
        return MP_OK; // asked what it would take, which the ABI allows
    }
    if (out_bytes < needed) {
        return MP_ERR_NO_MEMORY;
    }
    if (needed != 0) {
        std::memcpy(out, config.data(), needed);
    }
    return MP_OK;
}

/// The greatest common divisor, so a frame rate is reported as the ratio it
/// is rather than as two large numbers that happen to divide.
std::uint32_t gcd_of(std::uint64_t a, std::uint64_t b)
{
    while (b != 0) {
        const std::uint64_t t = a % b;
        a = b;
        b = t;
    }
    return static_cast<std::uint32_t>(a);
}

MpResult MP_CALL demux_stream_video_info(MpDemux* d, std::uint32_t index,
                                         MpVideoInfo* out) noexcept
try {
    if (d == nullptr || out == nullptr || index >= d->streams.size()) {
        return MP_ERR_INVALID;
    }
    if (d->streams[index].kind != MP_STREAM_VIDEO) {
        return MP_ERR_UNSUPPORTED;
    }
    AP4_Track* track = d->streams[index].track;
    AP4_SampleDescription* desc = track != nullptr ? track->GetSampleDescription(0) : nullptr;
    auto* video = AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, desc);
    if (video == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }

    const std::uint32_t size = out->size;
    *out = MpVideoInfo{};
    out->size = size;
    out->width = video->GetWidth();
    out->height = video->GetHeight();

    // **`tkhd` is the display size and the sample entry is the coded one**, and
    // they differ whenever the pixels are not square -- anamorphic DVD-era
    // content, and anything a phone rotated. Both are 16.16 fixed point here.
    out->display_width = track->GetWidth() >> 16;
    out->display_height = track->GetHeight() >> 16;
    if (out->display_width == 0 || out->display_height == 0) {
        out->display_width = out->width;
        out->display_height = out->height;
    }

    // The average, as a ratio, from the two numbers the container states
    // exactly: 24000/1001 comes back as 24000/1001 rather than as 23.976.
    const std::uint64_t frames = track->GetSampleCount();
    const std::uint64_t duration = track->GetMediaDuration();
    const std::uint64_t scale = track->GetMediaTimeScale();
    if (frames != 0 && duration != 0 && scale != 0) {
        const std::uint64_t num = frames * scale;
        const std::uint32_t g = gcd_of(num, duration);
        if (g != 0 && num / g <= 0xFFFFFFFFull && duration / g <= 0xFFFFFFFFull) {
            out->fps_num = static_cast<std::uint32_t>(num / g);
            out->fps_den = static_cast<std::uint32_t>(duration / g);
        }
    }

    // **`colr`, which is the whole reason this call exists.** Nothing else says
    // whether the frames are BT.709 or BT.2020, or whether the transfer is sRGB
    // or PQ, and a renderer that guesses produces a picture that is merely
    // plausible. 2 is "unspecified" in all three, which is what a file with no
    // `colr` leaves and what MpVideoInfo starts at.
    out->primaries = 2;
    out->transfer = 2;
    out->matrix = 2;
    if (desc != nullptr) {
        const AP4_AtomParent& details = desc->GetDetails();
        if (auto* colr = AP4_DYNAMIC_CAST(AP4_ColrAtom,
                                          details.GetChild(AP4_ATOM_TYPE_COLR))) {
            // `nclx` and `nclc` carry the code points; `rICC`/`prof` carry an
            // ICC profile instead, and this module has nowhere to put one.
            const AP4_UI32 kind = colr->GetColourParameterType();
            if (kind == AP4_ATOM_TYPE('n', 'c', 'l', 'x') ||
                kind == AP4_ATOM_TYPE('n', 'c', 'l', 'c')) {
                out->primaries = colr->GetPrimariesIndex();
                out->transfer = colr->GetTransferFunctionIndex();
                out->matrix = colr->GetMatrixIndex();
                if (colr->GetFullRangeFlag() != 0) {
                    out->flags |= MP_VIDEO_FULL_RANGE;
                }
            }
        }
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_select_streams(MpDemux* d, const std::uint32_t* indices,
                                      std::uint32_t count) noexcept
try {
    if (d == nullptr || indices == nullptr || count == 0) {
        return MP_ERR_INVALID;
    }
    std::vector<std::size_t> chosen;
    chosen.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (indices[i] >= d->streams.size()) {
            return MP_ERR_INVALID;
        }
        // A stream named twice would be enabled twice and read twice, which is
        // a caller's bug rather than a request.
        for (const std::size_t already : chosen) {
            if (already == indices[i]) {
                return MP_ERR_INVALID;
            }
        }
        chosen.push_back(indices[i]);
    }
    d->selected = std::move(chosen);
    return restart(d, nullptr, 0) ? MP_OK : MP_ERR_UNSUPPORTED;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_read_packet(MpDemux* d, void* dst, std::size_t dst_bytes,
                                   MpPacket* out) noexcept
try {
    if (d == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    out->bytes = 0;
    if (d->reader == nullptr) {
        return MP_ERR_INVALID;
    }

    if (!d->have_pending) {
        if (d->at_end) {
            return MP_END;
        }
        AP4_Sample sample;
        d->stream->arm(k_packet_budget);
        // **In storage order, from any enabled track**, which is the overload
        // that makes one pass over the file serve every selected stream. The
        // track id comes back with the sample, and is how a host is told which
        // stream it is holding.
        AP4_UI32 track_id = 0;
        const AP4_Result r = d->reader->ReadNextSample(sample, d->sample_data, track_id);
        if (r == AP4_ERROR_EOS) {
            d->at_end = true;
            return MP_END;
        }
        if (AP4_FAILED(r)) {
            return MP_ERR_IO;
        }
        const std::size_t from = stream_of_track(d, track_id);
        if (from == static_cast<std::size_t>(-1)) {
            return MP_ERR_INTERNAL; // a track this module never enabled
        }
        d->pending_stream = static_cast<std::uint32_t>(from);
        // **The decode timestamp is the frame number.** For audio it is stated
        // in the media timescale, which is the sample rate, so nothing stands
        // between the file and `MpPacket::frame` -- and no assumption that
        // every packet is the same length, which the last one of a track is not.
        d->pending_frame = sample.GetDts();
        d->have_pending = true;
    }

    const AP4_Size bytes = d->sample_data.GetDataSize();
    if (dst == nullptr || dst_bytes < bytes) {
        // **Nothing is consumed.** The host grows its buffer and asks again,
        // which is the only way a packet larger than somebody's guess is not
        // silently lost. The sample stays read, so the file is not walked twice.
        out->bytes = bytes;
        return MP_ERR_NO_MEMORY;
    }
    if (bytes != 0) {
        std::memcpy(dst, d->sample_data.GetData(), bytes);
    }
    out->bytes = bytes;
    out->flags = MP_PACKET_SYNC | MP_PACKET_TIMED;
    out->frame = d->pending_frame;
    out->stream = d->pending_stream;
    d->have_pending = false;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL demux_seek(MpDemux* d, std::uint32_t stream,
                            std::uint64_t frame) noexcept
try {
    if (d == nullptr || stream >= d->streams.size() || d->selected.empty()) {
        return MP_ERR_INVALID;
    }
    // **`stream` need not be selected.** A host seeking by the audio clock
    // names the audio stream whether or not it is reading it, and `frame` is in
    // that stream's own rate either way.
    AP4_Track* track = d->streams[stream].track;
    if (track == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }

    // **To the sample containing the frame**, which is the nearest point the
    // codec can be started from. `stts` says which one that is exactly, and
    // never past it -- what precedes the target inside that sample is the
    // host's to discard, and `MP_PACKET_TIMED` is what lets it.
    AP4_SampleTable* table = track->GetSampleTable();
    AP4_Ordinal index = 0;
    if (table != nullptr &&
        AP4_SUCCEEDED(table->GetSampleIndexForTimeStamp(frame, index))) {
        // Only the named track is placed. The others come from wherever
        // `SetSampleIndex` left the file, which is what an interleaved
        // container can do -- each arrives from its own nearest point and the
        // host discards what precedes its own target.
        return restart(d, track, index) ? MP_OK : MP_ERR_IO;
    }

    // A fragmented file has no sample table to ask, so the reader is asked
    // instead. It takes milliseconds, so the target is rounded *down* to one:
    // landing early costs the host a discard, landing late loses audio.
    const std::uint32_t rate = track->GetMediaTimeScale();
    if (rate == 0 || !restart(d, nullptr, 0)) {
        return MP_ERR_UNSUPPORTED;
    }
    const auto ms = static_cast<std::uint32_t>(frame * 1000ull / rate);
    return AP4_SUCCEEDED(d->reader->SeekTo(ms)) ? MP_OK : MP_ERR_UNSUPPORTED;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL demux_close(MpDemux* d) noexcept
{
    // The reader points into the movie and the movie into the file, so they go
    // in that order -- which is what the member order gives, in reverse.
    delete d;
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
    /* read_frames   */ nullptr, // it splits properly, so it does not decode
    /* close         */ &demux_close,
    /* stream_video_info */ &demux_stream_video_info,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

/// What it can produce, as data. A container carries what it carries, so this
/// is what a report may say rather than what the file will hold.
const MpCodec g_codecs[] = {MP_CODEC_ALAC, MP_CODEC_AAC_LC};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 2, 0),
    /* kind        */ MP_KIND_DEMUX,
    /* priority    */ 100,
    /* id          */ "demux_mp4",
    /* name        */ "MP4, QuickTime and fragmented MP4 (Bento4)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
    /* codecs      */ g_codecs,
    /* codec_count */ 2,
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

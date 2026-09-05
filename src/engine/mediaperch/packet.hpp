// SPDX-License-Identifier: GPL-3.0-or-later
//
// A container and a codec, joined into the source the graph already knows.
//
// **This is what replaces "try the decoders in order".** A demuxer identifies
// the file and says what is in it; the stream names its codec; the codec is
// looked up rather than guessed at. What comes out the far end is an `ISource`,
// so nothing below this changes -- the graph, the
// ring and the sink never learn that anything moved.
//
// Three things live here that used to live in every decoder separately, and
// gathering them is most of the reason for the split:
//
//  * **The gapless edit.** `skip_frames` and `play_frames` come from
//    `MpStreamInfo`, because they were always the container's: `elst` in MP4,
//    the LAME tag in an MP3's first frame, `pre_skip` in an Opus header. Three
//    modules each did this and each could be wrong on its own.
//  * **Seeking's second half.** A demuxer seeks to a packet; a codec has to be
//    told to forget, and then given the packets before the target so its state
//    is warm -- AAC's priming frame, MP3's bit reservoir. v1 hid that inside
//    each decoder; here it is written once.
//  * **Growing the packet buffer.** A packet that does not fit is not lost: the
//    demuxer says what it needs and nothing is consumed.
//
// A stream flagged `MP_STREAM_SELF_DECODES` skips the codec entirely and reads
// frames from the demuxer, which is how a pipeline that cannot be split --
// Media Foundation, FFmpeg through its own programs -- takes part without
// pretending to be two things.

#ifndef MEDIAPERCH_PACKET_HPP
#define MEDIAPERCH_PACKET_HPP

#include "mediaperch/format.hpp"
#include "mediaperch/source.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <span>
#include <vector>

namespace mp {

/// A demuxer module, opened on one file.
class Demux final {
public:
    Demux() noexcept = default;
    ~Demux() { close(); }

    Demux(const Demux&) = delete;
    Demux& operator=(const Demux&) = delete;
    Demux(Demux&&) = delete;
    Demux& operator=(Demux&&) = delete;

    /// `path` is UTF-8, as the ABI says.
    MpResult open(const MpDemuxVtbl& vtbl, const char* path);
    void close() noexcept;

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    [[nodiscard]] std::uint32_t stream_count() const noexcept { return streams_; }
    /// What the container says about one stream. False when there is no such
    /// stream or the module will not describe it.
    [[nodiscard]] bool stream_info(std::uint32_t index, MpStreamInfo& out) const;
    /// The codec's configuration blob, verbatim.
    [[nodiscard]] bool stream_config(std::uint32_t index, std::vector<std::uint8_t>& out) const;

    /// The stream the container marks as the one to play, or the first audio
    /// stream, or nothing. **A file is not one stream**, and picking is a
    /// decision rather than an accident, so it is made here and named.
    [[nodiscard]] bool best_audio_stream(std::uint32_t& out) const;

    /// Which streams `read_packet` may return. Selecting again replaces the
    /// set, and the set is never empty -- see the ABI header for why.
    MpResult select_streams(std::span<const std::uint32_t> indices);
    /// The one-stream case, which is every audio graph in this tree.
    MpResult select(std::uint32_t index) { return select_streams({&index, 1}); }
    /// Reads into `buffer`, growing it when the packet does not fit -- nothing
    /// is consumed by a read that could not deliver. MP_END at the end.
    /// `out.stream` says which stream it came from.
    MpResult read_packet(std::vector<std::uint8_t>& buffer, MpPacket& out);
    /// To `frame` of `stream`, in that stream's own rate. Moves every selected
    /// stream, because one file has one position.
    MpResult seek(std::uint32_t stream, std::uint64_t frame);
    /// Whether this module implements seeking at all. A stream arriving down a
    /// pipe has no `seek` in its vtable, and asking is how a source reports
    /// that rather than failing one later.
    [[nodiscard]] bool can_seek() const noexcept
    {
        return vtbl_ != nullptr && vtbl_->seek != nullptr;
    }
    /// What the container says about a video stream. False for an audio stream,
    /// and false on a demuxer with no video in it.
    [[nodiscard]] bool video_info(std::uint32_t index, MpVideoInfo& out) const;
    /// Only for MP_STREAM_SELF_DECODES.
    MpResult read_frames(void* dst, std::size_t bytes, std::size_t& out_bytes);

    [[nodiscard]] const MpDemuxVtbl* vtbl() const noexcept { return vtbl_; }

private:
    const MpDemuxVtbl* vtbl_ = nullptr;
    MpDemux* handle_ = nullptr;
    std::uint32_t streams_ = 0;
};

/// A codec module, opened on one stream's configuration.
class Codec final {
public:
    Codec() noexcept = default;
    ~Codec() { close(); }

    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    Codec(Codec&&) = delete;
    Codec& operator=(Codec&&) = delete;

    MpResult open(const MpCodecVtbl& vtbl, MpCodec codec, const std::uint8_t* config,
                  std::uint32_t config_bytes);
    void close() noexcept;

    explicit operator bool() const noexcept { return vtbl_ != nullptr && handle_ != nullptr; }

    /// What this codec produces. Known after `open` for a codec whose
    /// configuration states it, and after the first packet otherwise.
    [[nodiscard]] bool format(Format& out) const;

    MpResult decode(const void* packet, std::size_t packet_bytes, void* dst,
                    std::size_t dst_bytes, std::size_t& out_bytes);
    MpResult flush(void* dst, std::size_t dst_bytes, std::size_t& out_bytes);
    MpResult reset();

private:
    const MpCodecVtbl* vtbl_ = nullptr;
    MpCodecInstance* handle_ = nullptr;
};

/// Where one stream's packets come from.
///
/// An interface rather than a demuxer, because §4 says **one file has one
/// position**: audio and video out of the same file come from one demuxer with
/// both streams selected, and something has to read it once and route what
/// comes out. `PacketRouter` is that something. A caller reading a single
/// stream needs none of it and implements this in four lines.
class IPacketFeed {
public:
    IPacketFeed() = default;
    IPacketFeed(const IPacketFeed&) = delete;
    IPacketFeed& operator=(const IPacketFeed&) = delete;
    IPacketFeed(IPacketFeed&&) = delete;
    IPacketFeed& operator=(IPacketFeed&&) = delete;
    virtual ~IPacketFeed() = default;

    /// The next packet for this stream. `buffer` is grown as needed and
    /// `out.bytes` says how much of it is the packet.
    ///
    /// MP_END at the end of the stream. **MP_ERR_BUSY when somebody else has
    /// to read first** -- see `PacketRouter` for what that means and why it is
    /// not an error.
    virtual MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) = 0;
};

/// One demuxer, read once, feeding every stream that was selected.
///
/// **The alternative is two file positions**, and the ABI header says what is
/// wrong with it in as many words: opening the container twice means two
/// positions, and a seek then has to move both and land them on the same
/// moment. One demuxer cannot be got wrong that way -- `seek` here moves the
/// file, and every consumer's queue is emptied because everything in it is
/// from before.
///
/// The cost is that a packet read for one consumer while another is asking has
/// to wait somewhere. That is what the queues are, and they are the only
/// buffering: a consumer asking for its own stream reads **straight into its
/// own buffer** when the next packet in the file happens to be its own, which
/// for an interleaved file is most of the time.
///
/// **Back pressure, not silence.** A queue that grew without bound would turn
/// a badly interleaved file, or a consumer that stopped asking, into memory
/// exhaustion. So there is a cap, and reaching it makes the *other* consumer's
/// `next` answer MP_ERR_BUSY -- "somebody has to drain before I can read
/// more". Dropping the packets instead would be silent corruption, and
/// blocking would be a deadlock in a player whose two consumers are on
/// different threads.
/// How much a router may hold for a consumer that is not asking.
///
/// At namespace scope rather than nested inside PacketRouter, for the reason
/// PassthroughConfig gives: a default argument of a nested type needs that
/// type's default member initializers while the enclosing class is still
/// incomplete. MSVC accepts it; clang does not, and clang is right. This tree
/// has now made the same mistake twice, which is what the second front end is
/// for -- it caught it before the build did.
struct PacketRouterLimits {
    /// How many bytes of one stream may wait for a consumer that is not
    /// asking. Exceeded by at most one packet, because a packet already read
    /// cannot be put back.
    ///
    /// Thirty-two megabytes is far past any sane interleave -- a second of 4K
    /// video is a few -- and small enough that a file which is not sane says so
    /// rather than filling memory.
    std::size_t queued_bytes_per_stream = 32u * 1024u * 1024u;

    /// How many packets may wait before the byte cap is allowed to bite.
    ///
    /// **A byte cap alone is a resolution limit in disguise.** An eight-bit
    /// 4:2:0 frame at 16K is 190 MB uncompressed, so a keyframe out of one can
    /// be a good fraction of the byte cap on its own -- and a cap that a single
    /// packet exceeds turns "wait for the other consumer" into "wait after
    /// every packet". The measured ceiling is Direct3D's 16384 and nothing
    /// above the decoder has a resolution in it, so the cap should not quietly
    /// acquire one either.
    ///
    /// So a queue is full only when it is over the bytes **and** holding at
    /// least this many, which bounds what waits at about this many of the
    /// largest packet the file has rather than at a number chosen years
    /// earlier. Four, because one is enough for the file to keep moving and
    /// four is few enough round trips to be quiet on an ordinary interleave --
    /// where the byte cap is what bites anyway, four packets of audio being a
    /// few hundred bytes.
    std::uint32_t queued_packets_floor = 4;
};

class PacketRouter final {
public:
    using Limits = PacketRouterLimits;

    /// `demux` must already have these streams selected; the router does not
    /// select for you, because selecting is what says a file is not one stream
    /// and that decision belongs to whoever opened it.
    PacketRouter(Demux& demux, std::span<const std::uint32_t> streams,
                 PacketRouterLimits limits = {});

    PacketRouter(const PacketRouter&) = delete;
    PacketRouter& operator=(const PacketRouter&) = delete;
    PacketRouter(PacketRouter&&) = delete;
    PacketRouter& operator=(PacketRouter&&) = delete;

    /// The feed for one stream. Valid as long as the router is, and the same
    /// object every time. Null for a stream this router was not given.
    [[nodiscard]] IPacketFeed* feed(std::uint32_t stream) noexcept;

    /// The next packet for `stream`. What `IPacketFeed::next` calls.
    MpResult next(std::uint32_t stream, std::vector<std::uint8_t>& buffer, MpPacket& out);

    /// Moves the file and empties every queue. `stream` names the stream the
    /// target is counted in, exactly as `Demux::seek` does.
    ///
    /// **One call, because the two halves cannot be separated**: a seek that
    /// left the queues alone would hand a consumer packets from before it.
    MpResult seek(std::uint32_t stream, std::uint64_t frame);

    /// Empties every queue and forgets the end of the stream. What `seek` does
    /// after moving the file, and what a caller that moved it another way has
    /// to do itself.
    void clear() noexcept;

    /// The demuxer, for the questions that are not reading.
    ///
    /// **`stream_info`, `stream_config`, `video_info` and the rest describe the
    /// file and move nothing**, so asking them behind the router's back is
    /// harmless and forcing them through it would only be a second spelling.
    /// `read_packet` and `seek` are the two that move the position, and those
    /// are the router's -- calling them here is what this class exists to stop.
    [[nodiscard]] Demux& demux() noexcept { return *demux_; }

    struct Stats {
        /// Waiting for a consumer that has not asked.
        std::size_t queued_bytes = 0;
        std::size_t queued_packets = 0;
        /// The most that ever waited, which is the number that says whether a
        /// file's interleave is sane.
        std::size_t peak_queued_bytes = 0;
        /// Packets read from the demuxer, and how many of those had to wait
        /// for somebody. The difference is how often a consumer's own packet
        /// was the next one in the file.
        std::uint64_t read = 0;
        std::uint64_t queued = 0;
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }

private:
    /// One packet, waiting.
    struct Held {
        std::vector<std::uint8_t> bytes;
        MpPacket info{};
    };

    struct Queue;

    /// The `IPacketFeed` face of one stream. Separate from `Queue` only so
    /// that the vtable and the data are not the same object's problem.
    class Feed final : public IPacketFeed {
    public:
        Feed(PacketRouter& router, std::uint32_t stream) noexcept
            : router_(&router), stream_(stream)
        {
        }
        MpResult next(std::vector<std::uint8_t>& buffer, MpPacket& out) override
        {
            return router_->next(stream_, buffer, out);
        }

    private:
        PacketRouter* router_;
        std::uint32_t stream_;
    };

    struct Queue {
        Queue(PacketRouter& router, std::uint32_t index) : stream(index), feed(router, index)
        {
        }
        std::uint32_t stream;
        std::deque<Held> waiting;
        std::size_t bytes = 0;
        Feed feed;
    };

    [[nodiscard]] Queue* find(std::uint32_t stream) noexcept;
    /// Whether any queue other than `mine` is full -- over the byte cap *and*
    /// past the packet floor -- which is when reading another packet could push
    /// it further over.
    [[nodiscard]] bool someone_is_full(const Queue* mine) const noexcept;
    /// A vector with capacity, from the ones packets have already been in.
    [[nodiscard]] std::vector<std::uint8_t> spare();
    void recycle(std::vector<std::uint8_t>&& used);

    Demux* demux_;
    PacketRouterLimits limits_;
    /// `unique_ptr` because a `Queue` holds its own `Feed`, whose address a
    /// caller keeps: nothing may move it, and a vector of them would.
    std::vector<std::unique_ptr<Queue>> queues_;
    /// The demuxer said MP_END. A queue with packets in it still has them.
    bool ended_ = false;
    std::vector<std::vector<std::uint8_t>> spares_;
    Stats stats_{};
};

/// A demuxer and a codec, seen by the graph as one source.
class PacketSource final : public ISource {
public:
    PacketSource() noexcept = default;
    ~PacketSource() override = default;

    PacketSource(const PacketSource&) = delete;
    PacketSource& operator=(const PacketSource&) = delete;
    PacketSource(PacketSource&&) = delete;
    PacketSource& operator=(PacketSource&&) = delete;

    /// Opens the demuxer, picks the audio stream, and opens the codec for it.
    ///
    /// `find_codec` is how the host answers "which module decodes this" -- by
    /// looking the id up, not by trying anything. It may return nullptr, which
    /// is an honest "nobody here decodes that" and is why the container being
    /// readable and the codec being playable are two different questions.
    /// **The configuration is part of the question.** A codec's probe is given
    /// the container's blob because that is how it declines a stream it cannot
    /// take -- HE-AAC in an AAC-LC decoder, an ALAC cookie describing a depth
    /// nobody implements. Asking with the id alone gets a "no" from a module
    /// that would have said yes.
    using FindCodec =
        std::function<const MpCodecVtbl*(MpCodec, const std::uint8_t*, std::uint32_t)>;
    bool open(const MpDemuxVtbl& demux, const char* path, const FindCodec& find_codec,
              std::string& why);

    /// Reads one stream through a router that somebody else opened.
    ///
    /// **The same source, fed differently.** Everything below this class --
    /// the gapless edit, the seek warm-up, the trim -- is the same code and the
    /// same behaviour; what changes is where the packets come from, which is
    /// what §4's one-file-one-position asks for when audio and video are both
    /// being read. `stream` must be one the router was given, and the caller
    /// has already selected it on the demuxer.
    ///
    /// A stream flagged `MP_STREAM_SELF_DECODES` is refused here: a demuxer
    /// that decodes for itself hands over frames rather than packets, and there
    /// is nothing for a router to route.
    bool open(PacketRouter& router, std::uint32_t stream, const FindCodec& find_codec,
              std::string& why);

    [[nodiscard]] const Format& format() const noexcept override { return format_; }
    std::size_t read(void* dst, std::size_t bytes) override;

    [[nodiscard]] bool seekable() const noexcept override { return seekable_; }
    bool seek(std::uint64_t frame) override;
    [[nodiscard]] std::uint64_t length_frames() const noexcept override { return length_; }

    [[nodiscard]] const MpStreamInfo& stream() const noexcept { return stream_; }
    /// Whether the demuxer decoded this itself rather than a codec module.
    [[nodiscard]] bool self_decoded() const noexcept { return self_decodes_; }

private:
    /// The half of `open` that is the same whoever supplies the packets.
    bool finish_open(Demux& demux, std::uint32_t index, const FindCodec& find_codec,
                     std::string& why);
    /// Fills `pcm_` with at least one packet's worth, or reports the end.
    bool pump();
    /// After a seek: forget, then feed what precedes the target so the codec's
    /// state is warm before anything is kept.
    void warm_up(std::uint64_t target);

    /// This class's own demuxer, opened by the `open` that takes a path. Unused
    /// when a router is feeding it: then the file belongs to whoever opened it.
    Demux demux_;
    /// Where packets come from and what moves the file, when they are not this
    /// class's own demuxer's. Both null or both set.
    PacketRouter* router_ = nullptr;
    IPacketFeed* feed_ = nullptr;
    Codec codec_;
    MpStreamInfo stream_{};
    Format format_{};
    std::uint64_t length_ = 0;
    bool seekable_ = false;
    bool self_decodes_ = false;
    bool drained_ = false;

    std::vector<std::uint8_t> packet_;
    /// Decoded bytes not yet handed to the caller.
    std::vector<std::uint8_t> pcm_;
    std::size_t pcm_at_ = 0;

    /// The last `trim_` frames decoded, held back rather than emitted, because
    /// until another packet arrives they may be the end of the stream. Rides at
    /// the front of `pcm_` on the next pump. Never longer than `trim_` frames.
    std::vector<std::uint8_t> carry_;

    /// Where a seek asked to land, in the stream's own frames, and whether the
    /// next packet is the first one after it.
    ///
    /// **A demuxer seeks to a packet, not to a frame**, and it may deliberately
    /// seek further back still so the codec has its pre-roll. Both leave audio
    /// in front of the target, and discarding it is the host's -- once, here,
    /// rather than once per module and differently each time.
    std::uint64_t seek_target_ = 0;
    bool after_seek_ = false;

    /// Frames of the gapless edit still to discard, and still to emit.
    std::uint64_t skip_ = 0;
    std::uint64_t remaining_ = 0;
    bool bounded_ = false;
    /// `MpStreamInfo::trim_frames`: how many frames at the end of the decoded
    /// stream are the encoder's padding. Capped at one second on the way in.
    std::uint64_t trim_ = 0;
    /// Where the caller is, in the stream's frames.
    std::uint64_t position_ = 0;
};

} // namespace mp

#endif // MEDIAPERCH_PACKET_HPP

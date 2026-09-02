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
#include <functional>
#include <string>
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

    MpResult select(std::uint32_t index);
    /// Reads into `buffer`, growing it when the packet does not fit -- nothing
    /// is consumed by a read that could not deliver. MP_END at the end.
    MpResult read_packet(std::vector<std::uint8_t>& buffer, MpPacket& out);
    MpResult seek(std::uint64_t frame);
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

    [[nodiscard]] const Format& format() const noexcept override { return format_; }
    std::size_t read(void* dst, std::size_t bytes) override;

    [[nodiscard]] bool seekable() const noexcept override { return seekable_; }
    bool seek(std::uint64_t frame) override;
    [[nodiscard]] std::uint64_t length_frames() const noexcept override { return length_; }

    [[nodiscard]] const MpStreamInfo& stream() const noexcept { return stream_; }
    /// Whether the demuxer decoded this itself rather than a codec module.
    [[nodiscard]] bool self_decoded() const noexcept { return self_decodes_; }

private:
    /// Fills `pcm_` with at least one packet's worth, or reports the end.
    bool pump();
    /// After a seek: forget, then feed what precedes the target so the codec's
    /// state is warm before anything is kept.
    void warm_up(std::uint64_t target);

    Demux demux_;
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
    /// Where the caller is, in the stream's frames.
    std::uint64_t position_ = 0;
};

} // namespace mp

#endif // MEDIAPERCH_PACKET_HPP

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/packet.hpp"

#include <algorithm>
#include <cstring>

namespace mp {
namespace {

/// What a packet buffer starts at. Big enough for an ordinary audio packet --
/// an AAC frame is a couple of kilobytes, a FLAC frame a few -- and it grows on
/// demand for anything larger, so the number is a starting point rather than a
/// limit.
constexpr std::size_t k_packet_start = 8192;

/// How much decoded audio one packet may produce before something is wrong.
/// A frame of any codec here is at most a few thousand samples; this is two
/// orders of magnitude above that, so it bounds a module that lies without
/// getting in the way of one that does not.
constexpr std::size_t k_pcm_room = 1u << 20;

bool has(const MpDemuxVtbl& v, std::size_t offset) noexcept
{
    return v.size >= offset + sizeof(void*);
}

} // namespace

// --------------------------------------------------------------------------
// Demux
// --------------------------------------------------------------------------

MpResult Demux::open(const MpDemuxVtbl& vtbl, const char* path)
{
    close();
    if (vtbl.open == nullptr || vtbl.close == nullptr || path == nullptr) {
        return MP_ERR_INVALID;
    }
    MpDemux* handle = nullptr;
    const MpResult r = vtbl.open(path, &handle);
    if (r != MP_OK || handle == nullptr) {
        return r == MP_OK ? MP_ERR_INTERNAL : r;
    }
    vtbl_ = &vtbl;
    handle_ = handle;
    if (vtbl.stream_count != nullptr) {
        vtbl.stream_count(handle_, &streams_);
    }
    return MP_OK;
}

void Demux::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
    streams_ = 0;
}

bool Demux::stream_info(std::uint32_t index, MpStreamInfo& out) const
{
    if (!*this || vtbl_->stream_info == nullptr) {
        return false;
    }
    out = MpStreamInfo{};
    out.size = sizeof(MpStreamInfo);
    return vtbl_->stream_info(handle_, index, &out) == MP_OK;
}

bool Demux::stream_config(std::uint32_t index, std::vector<std::uint8_t>& out) const
{
    out.clear();
    if (!*this || vtbl_->stream_config == nullptr) {
        return false;
    }
    // Asked with no buffer first, which the ABI says is legal and is how a
    // caller learns a size it has no other way to know.
    std::uint32_t needed = 0;
    if (vtbl_->stream_config(handle_, index, nullptr, 0, &needed) != MP_OK) {
        return false;
    }
    if (needed == 0) {
        return true; // a codec whose configuration is the packets themselves
    }
    out.resize(needed);
    std::uint32_t written = 0;
    if (vtbl_->stream_config(handle_, index, out.data(), needed, &written) != MP_OK) {
        out.clear();
        return false;
    }
    out.resize(std::min<std::size_t>(written, out.size()));
    return true;
}

bool Demux::best_audio_stream(std::uint32_t& out) const
{
    // **A file is not one stream**, so this is a decision and it is written
    // down: what the container itself marks as default, and failing that the
    // first audio stream it lists. Anything cleverer -- language, channel
    // count, a person's preference -- belongs above this, where the person is.
    bool found = false;
    for (std::uint32_t i = 0; i < streams_; ++i) {
        MpStreamInfo info{};
        if (!stream_info(i, info) || info.kind != MP_STREAM_AUDIO) {
            continue;
        }
        if ((info.flags & MP_STREAM_DEFAULT) != 0) {
            out = i;
            return true;
        }
        if (!found) {
            out = i;
            found = true;
        }
    }
    return found;
}

bool Demux::video_info(std::uint32_t index, MpVideoInfo& out) const
{
    if (!*this || !has(*vtbl_, offsetof(MpDemuxVtbl, stream_video_info)) ||
        vtbl_->stream_video_info == nullptr) {
        return false;
    }
    out = MpVideoInfo{};
    out.size = sizeof(out);
    return vtbl_->stream_video_info(handle_, index, &out) == MP_OK;
}

MpResult Demux::select_streams(std::span<const std::uint32_t> indices)
{
    if (!*this || vtbl_->select_streams == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->select_streams(handle_, indices.data(),
                                 static_cast<std::uint32_t>(indices.size()));
}

MpResult Demux::read_packet(std::vector<std::uint8_t>& buffer, MpPacket& out)
{
    if (!*this || vtbl_->read_packet == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    if (buffer.empty()) {
        buffer.resize(k_packet_start);
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        out = MpPacket{};
        out.size = sizeof(MpPacket);
        const MpResult r = vtbl_->read_packet(handle_, buffer.data(), buffer.size(), &out);
        if (r != MP_ERR_NO_MEMORY) {
            return r;
        }
        // It did not fit and nothing was consumed, so the packet is still
        // there: grow to what it asked for and ask again. Twice at most,
        // because a module that asks for more than it then uses is a module
        // that would loop.
        if (out.bytes == 0 || out.bytes <= buffer.size()) {
            return MP_ERR_INTERNAL;
        }
        buffer.resize(out.bytes);
    }
    return MP_ERR_NO_MEMORY;
}

MpResult Demux::seek(std::uint32_t stream, std::uint64_t frame)
{
    if (!*this || vtbl_->seek == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->seek(handle_, stream, frame);
}

MpResult Demux::read_frames(void* dst, std::size_t bytes, std::size_t& out_bytes)
{
    out_bytes = 0;
    if (!*this || !has(*vtbl_, offsetof(MpDemuxVtbl, read_frames)) ||
        vtbl_->read_frames == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->read_frames(handle_, dst, bytes, &out_bytes);
}

// --------------------------------------------------------------------------
// Codec
// --------------------------------------------------------------------------

MpResult Codec::open(const MpCodecVtbl& vtbl, MpCodec codec, const std::uint8_t* config,
                     std::uint32_t config_bytes)
{
    close();
    if (vtbl.open == nullptr || vtbl.close == nullptr) {
        return MP_ERR_INVALID;
    }
    MpCodecInstance* handle = nullptr;
    const MpResult r = vtbl.open(codec, config, config_bytes, &handle);
    if (r != MP_OK || handle == nullptr) {
        return r == MP_OK ? MP_ERR_INTERNAL : r;
    }
    vtbl_ = &vtbl;
    handle_ = handle;
    return MP_OK;
}

void Codec::close() noexcept
{
    if (vtbl_ != nullptr && handle_ != nullptr && vtbl_->close != nullptr) {
        vtbl_->close(handle_);
    }
    vtbl_ = nullptr;
    handle_ = nullptr;
}

bool Codec::format(Format& out) const
{
    if (!*this || vtbl_->get_format == nullptr) {
        return false;
    }
    MpFormat raw{};
    if (vtbl_->get_format(handle_, &raw) != MP_OK) {
        return false;
    }
    out = from_abi(raw);
    return is_valid(out);
}

MpResult Codec::decode(const void* packet, std::size_t packet_bytes, void* dst,
                       std::size_t dst_bytes, std::size_t& out_bytes)
{
    out_bytes = 0;
    if (!*this || vtbl_->decode == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->decode(handle_, packet, packet_bytes, dst, dst_bytes, &out_bytes);
}

MpResult Codec::flush(void* dst, std::size_t dst_bytes, std::size_t& out_bytes)
{
    out_bytes = 0;
    if (!*this || vtbl_->flush == nullptr) {
        return MP_OK; // a codec that holds nothing has nothing to give back
    }
    return vtbl_->flush(handle_, dst, dst_bytes, &out_bytes);
}

MpResult Codec::reset()
{
    if (!*this || vtbl_->reset == nullptr) {
        return MP_ERR_UNSUPPORTED;
    }
    return vtbl_->reset(handle_);
}

// --------------------------------------------------------------------------
// PacketSource
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// PacketRouter
// --------------------------------------------------------------------------

PacketRouter::PacketRouter(Demux& demux, std::span<const std::uint32_t> streams,
                           PacketRouterLimits limits)
    : demux_(&demux), limits_(limits)
{
    queues_.reserve(streams.size());
    for (const std::uint32_t stream : streams) {
        if (find(stream) == nullptr) {
            queues_.push_back(std::make_unique<Queue>(*this, stream));
        }
    }
}

PacketRouter::Queue* PacketRouter::find(std::uint32_t stream) noexcept
{
    for (const std::unique_ptr<Queue>& queue : queues_) {
        if (queue->stream == stream) {
            return queue.get();
        }
    }
    return nullptr;
}

IPacketFeed* PacketRouter::feed(std::uint32_t stream) noexcept
{
    Queue* queue = find(stream);
    return queue != nullptr ? &queue->feed : nullptr;
}

bool PacketRouter::someone_is_full(const Queue* mine) const noexcept
{
    for (const std::unique_ptr<Queue>& queue : queues_) {
        if (queue.get() != mine && queue->bytes >= limits_.queued_bytes_per_stream) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> PacketRouter::spare()
{
    if (spares_.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out = std::move(spares_.back());
    spares_.pop_back();
    out.clear();
    return out;
}

void PacketRouter::recycle(std::vector<std::uint8_t>&& used)
{
    // A handful, because the point is to stop allocating a megabyte per
    // keyframe rather than to keep a pool. More than there are consumers is
    // more than can be in flight.
    if (spares_.size() < 4) {
        used.clear();
        spares_.push_back(std::move(used));
    }
}

MpResult PacketRouter::next(std::uint32_t stream, std::vector<std::uint8_t>& buffer,
                            MpPacket& out)
{
    Queue* mine = find(stream);
    if (mine == nullptr || demux_ == nullptr) {
        return MP_ERR_INVALID;
    }

    if (!mine->waiting.empty()) {
        // Swapped rather than copied: the packet was already put in a vector
        // once, and putting it in the caller's is a pointer exchange.
        Held held = std::move(mine->waiting.front());
        mine->waiting.pop_front();
        mine->bytes -= held.info.bytes;
        buffer.swap(held.bytes);
        out = held.info;
        recycle(std::move(held.bytes));
        stats_.queued_bytes -= held.info.bytes;
        --stats_.queued_packets;
        return MP_OK;
    }

    if (ended_) {
        return MP_END;
    }

    for (;;) {
        // Checked before reading, because a packet already read cannot be put
        // back: the cap is exceeded by at most the one packet that discovers
        // it, and never by a run of them.
        if (someone_is_full(mine)) {
            return MP_ERR_BUSY;
        }

        MpPacket packet{};
        const MpResult r = demux_->read_packet(buffer, packet);
        if (r == MP_END) {
            ended_ = true;
            return MP_END;
        }
        if (r != MP_OK) {
            return r;
        }
        ++stats_.read;

        if (packet.stream == stream) {
            // The common case for an interleaved file, and it cost no copy:
            // the demuxer read straight into the caller's own buffer.
            out = packet;
            return MP_OK;
        }

        Queue* other = find(packet.stream);
        if (other == nullptr) {
            // A stream the demuxer served that nobody selected. Not this
            // router's to keep, and dropping it is what `select_streams` asked
            // for.
            continue;
        }

        Held held;
        held.bytes = spare();
        held.bytes.swap(buffer); // held takes the packet, buffer takes the spare
        held.info = packet;
        other->bytes += packet.bytes;
        other->waiting.push_back(std::move(held));
        ++stats_.queued;
        ++stats_.queued_packets;
        stats_.queued_bytes += packet.bytes;
        if (stats_.queued_bytes > stats_.peak_queued_bytes) {
            stats_.peak_queued_bytes = stats_.queued_bytes;
        }
    }
}

MpResult PacketRouter::seek(std::uint32_t stream, std::uint64_t frame)
{
    if (demux_ == nullptr) {
        return MP_ERR_INVALID;
    }
    const MpResult r = demux_->seek(stream, frame);
    if (r == MP_OK) {
        clear();
    }
    return r;
}

void PacketRouter::clear() noexcept
{
    for (const std::unique_ptr<Queue>& queue : queues_) {
        queue->waiting.clear();
        queue->bytes = 0;
    }
    ended_ = false;
    stats_.queued_bytes = 0;
    stats_.queued_packets = 0;
}

bool PacketSource::open(const MpDemuxVtbl& demux, const char* path,
                        const FindCodec& find_codec, std::string& why)
{
    if (demux_.open(demux, path) != MP_OK) {
        why = "the container would not open";
        return false;
    }
    std::uint32_t index = 0;
    if (!demux_.best_audio_stream(index)) {
        why = "the container has no audio stream";
        return false;
    }
    if (!demux_.stream_info(index, stream_)) {
        why = "the container would not describe its audio stream";
        return false;
    }
    if (demux_.select(index) != MP_OK) {
        why = "the container would not select its audio stream";
        return false;
    }

    self_decodes_ = (stream_.flags & MP_STREAM_SELF_DECODES) != 0;
    if (!self_decodes_) {
        // **Looked up, not tried.** A container that says `MP_CODEC_ALAC` and a
        // build with no ALAC codec is a readable file nobody can play, and that
        // is a different sentence from "unreadable file" -- so it is a
        // different message.
        // The configuration first, because the lookup needs it: a codec module
        // decides from the blob whether this is a stream it can take.
        std::vector<std::uint8_t> config;
        (void)demux_.stream_config(index, config);
        const std::uint8_t* blob = config.empty() ? nullptr : config.data();
        const auto blob_bytes = static_cast<std::uint32_t>(config.size());

        const MpCodecVtbl* codec =
            find_codec ? find_codec(stream_.codec, blob, blob_bytes) : nullptr;
        if (codec == nullptr) {
            why = "nothing here decodes that codec";
            return false;
        }
        if (codec_.open(*codec, stream_.codec, blob, blob_bytes) != MP_OK) {
            why = "the codec would not open this stream";
            return false;
        }
    }

    // What the container said, and what the codec says when the container did
    // not. A container that states a format and a codec that contradicts it is
    // the codec's word: it is the one producing the samples.
    format_ = from_abi(stream_.format);
    Format from_codec{};
    if (!self_decodes_ && codec_.format(from_codec)) {
        format_ = from_codec;
    }
    if (!is_valid(format_)) {
        why = "neither the container nor the codec would say what the format is";
        return false;
    }

    length_ = stream_.play_frames != 0 ? stream_.play_frames : stream_.total_frames;
    skip_ = stream_.skip_frames;
    remaining_ = stream_.play_frames;
    bounded_ = stream_.play_frames != 0;

    // A trim is a codec frame's worth of padding at most, so a second of it is
    // already nonsense. Capped rather than trusted, because the number came out
    // of a file and an uncapped one would decide how much this holds in memory:
    // the held-back frames live here until the packets run out.
    trim_ = stream_.trim_frames <= format_.sample_rate ? stream_.trim_frames : 0;
    seekable_ = demux.seek != nullptr;
    return true;
}

bool PacketSource::pump()
{
    if (drained_) {
        return false;
    }
    // Whatever the last read held back goes in front of what is about to be
    // decoded, so the trim below sees one continuous stream rather than the end
    // of every packet.
    const std::size_t carried = carry_.size();
    pcm_.resize(carried + k_pcm_room);
    if (carried != 0) {
        std::memcpy(pcm_.data(), carry_.data(), carried);
        carry_.clear();
    }
    pcm_at_ = 0;
    std::uint8_t* const into = pcm_.data() + carried;
    const std::size_t room = k_pcm_room;

    if (self_decodes_) {
        // No packets, so nothing to discard against: such a demuxer seeks its
        // own way and the pipeline inside it lands where it was asked to.
        after_seek_ = false;
        std::size_t got = 0;
        const MpResult r = demux_.read_frames(into, room, got);
        pcm_.resize(carried + got);
        if (r != MP_OK || got == 0) {
            drained_ = true;
        }
        if (got == 0) {
            // The carry is the tail, and the tail is what the trim discards.
            pcm_.clear();
            return false;
        }
        return true;
    }

    for (;;) {
        MpPacket packet{};
        const MpResult r = demux_.read_packet(packet_, packet);
        if (r == MP_END || (r == MP_OK && packet.bytes == 0)) {
            // The end of the packets is not the end of the audio: a codec may
            // still be holding a frame.
            std::size_t got = 0;
            (void)codec_.flush(into, room, got);
            pcm_.resize(carried + got);
            drained_ = true;
            if (got == 0) {
                pcm_.clear();
                return false;
            }
            return true;
        }
        if (r != MP_OK) {
            pcm_.clear();
            drained_ = true;
            return false;
        }
        std::size_t got = 0;
        if (codec_.decode(packet_.data(), packet.bytes, into, room, got) != MP_OK) {
            pcm_.clear();
            drained_ = true;
            return false;
        }
        if (got != 0) {
            // **The first packet after a seek that produced anything says how
            // much of it precedes the target.** A demuxer seeks to the packet
            // containing a frame, or to one before it so a codec that needs
            // pre-roll gets some, and either way what comes back starts too
            // early.
            //
            // It has to be the first packet that *decoded*, not the first that
            // arrived: a codec fed a cold packet may hand back nothing at all --
            // an MPEG audio decoder does exactly that for the frame it has no
            // bit reservoir for -- and counting from a packet whose samples
            // never existed puts the discard a whole frame out.
            //
            // `MP_PACKET_TIMED` is the demuxer vouching for the number. Without
            // it the packet has no position and there is nothing to compute.
            if (after_seek_) {
                after_seek_ = false;
                if ((packet.flags & MP_PACKET_TIMED) != 0u && packet.frame < seek_target_) {
                    skip_ = seek_target_ - packet.frame;
                }
            }
            pcm_.resize(carried + got);
            return true;
        }
        // A packet that decoded to nothing -- a priming frame. Ask for another
        // rather than reporting an end that has not happened.
    }
}

std::size_t PacketSource::read(void* dst, std::size_t bytes)
{
    const std::size_t stride = frame_bytes(format_);
    if (stride == 0 || dst == nullptr) {
        return 0;
    }
    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t filled = 0;

    while (filled + stride <= bytes) {
        if (pcm_at_ >= pcm_.size()) {
            if (!pump()) {
                break;
            }
            continue;
        }

        // The gapless edit, applied here rather than in three decoders. The
        // encoder's warm-up is discarded before anything is counted, and the
        // file's own length is what stops it.
        if (skip_ != 0) {
            const std::size_t available = (pcm_.size() - pcm_at_) / stride;
            const std::size_t drop = static_cast<std::size_t>(
                std::min<std::uint64_t>(skip_, available));
            pcm_at_ += drop * stride;
            skip_ -= drop;
            continue;
        }
        if (bounded_ && remaining_ == 0) {
            break;
        }

        // **The tail trim, which is a count rather than a length.** The last
        // `trim_` frames buffered are never emitted: until another packet
        // arrives there is no way to know they are not the end of the stream,
        // and if they are, they are the encoder's padding. So they are carried
        // forward, and when the packets run out they are simply never handed
        // over. See `MpStreamInfo::trim_frames` for why this cannot be done by
        // shortening the length instead.
        const std::size_t buffered = pcm_.size() - pcm_at_;
        if (trim_ != 0 && buffered / stride <= trim_) {
            carry_.assign(pcm_.begin() + static_cast<std::ptrdiff_t>(pcm_at_), pcm_.end());
            pcm_.clear();
            pcm_at_ = 0;
            if (!pump()) {
                break;
            }
            continue;
        }

        std::size_t room = bytes - filled;
        room -= room % stride;
        std::size_t take = std::min(room, buffered);
        if (trim_ != 0) {
            take = std::min(take, buffered - static_cast<std::size_t>(trim_) * stride);
        }
        take -= take % stride;
        if (bounded_) {
            const std::uint64_t allowed = remaining_ * stride;
            take = static_cast<std::size_t>(std::min<std::uint64_t>(take, allowed));
        }
        if (take == 0) {
            break;
        }
        std::memcpy(out + filled, pcm_.data() + pcm_at_, take);
        pcm_at_ += take;
        filled += take;
        const std::uint64_t frames = take / stride;
        position_ += frames;
        if (bounded_) {
            remaining_ -= frames;
        }
    }
    return filled;
}

void PacketSource::warm_up(std::uint64_t target)
{
    // The codec forgets, and then is fed what precedes the target so its state
    // is warm before anything is kept: AAC's priming frame, MP3's bit
    // reservoir. v1 hid this inside each decoder, which is why each one had to
    // be right about it separately.
    (void)codec_.reset();
    pcm_.clear();
    pcm_at_ = 0;
    carry_.clear();
    drained_ = false;
    position_ = target;
}

bool PacketSource::seek(std::uint64_t frame)
{
    if (!seekable_) {
        return false;
    }
    // The edit is measured from the start of the *stream*, so a seek past it
    // has nothing left to discard and a seek before it still does.
    const std::uint64_t absolute = frame + stream_.skip_frames;
    if (demux_.seek(stream_.index, absolute) != MP_OK) {
        return false;
    }
    warm_up(frame);
    skip_ = 0;
    seek_target_ = absolute;
    after_seek_ = true;
    if (bounded_) {
        remaining_ = stream_.play_frames > frame ? stream_.play_frames - frame : 0;
    }
    return true;
}

} // namespace mp

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gapless playback, as a source rather than as a special case.
//
// **Nothing in the graph knows this exists.** A queue is an `ISource` whose
// `read` does not return zero at the end of a track -- it opens the next one
// and keeps filling the same buffer. The ring never runs dry, the render thread
// never learns that anything happened, and the device never stops. That is what
// gapless is: not a feature bolted to the transport, but the absence of a stop.
//
// The alternative -- ending one graph and starting another -- cannot be
// gapless in exclusive mode at all. `IAudioClient::Stop` and a fresh
// `Initialize` take milliseconds, the device silences the moment the first one
// releases it, and no amount of care on this side closes that. So the seam has
// to be somewhere the device does not see, and the only such place is upstream
// of the ring.
//
// **Two things it will not do.** It will not join tracks whose formats differ:
// 44.1 kHz followed by 96 kHz needs the device renegotiated, which is a new
// graph and an audible gap, so the queue stops and says why rather than
// resampling something nobody asked it to. And it does not remove the encoder's
// padding -- that is the decoder's, and `decode_mp3` reads the LAME tag for
// exactly this reason.

#ifndef MEDIAPERCH_QUEUE_HPP
#define MEDIAPERCH_QUEUE_HPP

#include "mediaperch/source.hpp"

#include <cstdint>

namespace mp {

/// Why a queue stopped, when it stopped before the playlist ended.
enum class QueueStop : std::uint32_t {
    /// It has not stopped, or it reached the end of the playlist.
    end,
    /// The next item is in a different format. The host has to rebuild the
    /// graph around it, and there will be a gap because the device says so.
    format_change,
    /// The next item would not open. Not fatal to the queue -- it is skipped --
    /// but recorded, because a playlist that silently plays four of its five
    /// entries is worse than one that says which.
    unreadable,
};

class Queue final : public ISource {
public:
    /// `playlist` supplies the sources and outlives the queue. `first` is where
    /// to start, so a host that has just been forced to rebuild for a format
    /// change can carry on from the item that caused it.
    Queue(IPlaylist& playlist, std::size_t first = 0);

    [[nodiscard]] const Format& format() const noexcept override { return format_; }
    std::size_t read(void* dst, std::size_t bytes) override;

    [[nodiscard]] bool seekable() const noexcept override;
    bool seek(std::uint64_t frame) override;
    /// The current item's length, not the playlist's: a queue's total is a
    /// question about a playlist and this is a question about a track.
    [[nodiscard]] std::uint64_t length_frames() const noexcept override;

    /// Opens the first item and takes its format. Until this succeeds the queue
    /// has no format and cannot be given to a graph.
    [[nodiscard]] bool open(std::string& why);

    /// Which item is playing, and how far into it, in that item's own frames.
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    /// How many items have been played through to their end.
    [[nodiscard]] std::size_t completed() const noexcept { return completed_; }
    /// Why it stopped, and which item it stopped on.
    [[nodiscard]] QueueStop stopped() const noexcept { return stopped_; }
    /// The format the next item wants, when `stopped()` is `format_change`.
    [[nodiscard]] const Format& next_format() const noexcept { return next_format_; }

    /// Abandons the rest of the current item and moves on. What a "next track"
    /// button is: the queue is asked, not the graph, and the device never
    /// notices.
    void skip() noexcept { skip_ = true; }

private:
    /// Opens item `index_ + 1`, or reports why it will not.
    [[nodiscard]] bool advance();

    IPlaylist* playlist_;
    ISource* current_ = nullptr;
    Format format_{};
    Format next_format_{};
    std::size_t index_ = 0;
    std::size_t completed_ = 0;
    std::uint64_t position_ = 0;
    QueueStop stopped_ = QueueStop::end;
    bool skip_ = false;
    bool done_ = false;
};

} // namespace mp

#endif // MEDIAPERCH_QUEUE_HPP

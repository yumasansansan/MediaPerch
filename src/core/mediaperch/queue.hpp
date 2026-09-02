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

#include <atomic>
#include <cstdint>
#include <vector>

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
    [[nodiscard]] std::uint64_t item_position() const noexcept;

    /// How far into the *queue*: frames handed out since it was built, counted
    /// straight through every boundary.
    ///
    /// This is the coordinate `seek` speaks, and it is this one rather than the
    /// track's because of who the other party is. A graph counts what the
    /// device played; it has never heard of a track boundary and never will,
    /// since not seeing one is what gapless *is*. So the queue and the graph
    /// need a number they can both name, and only the queue can convert it back
    /// into a track and an offset.
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
    ///
    /// Atomic because the asking is done by whichever thread a shell is on and
    /// the reading by the decode thread.
    void skip() noexcept { skip_.store(true, std::memory_order_release); }

    /// The queue frame at which the current item began, and the one before it.
    /// What a "previous track" button needs, and it is a question only the
    /// queue can answer: a graph counts frames and has never seen a boundary.
    [[nodiscard]] std::uint64_t item_start() const noexcept;
    [[nodiscard]] bool has_previous() const noexcept;
    [[nodiscard]] std::uint64_t previous_start() const noexcept;

private:
    /// Opens item `index_ + 1`, or reports why it will not.
    [[nodiscard]] bool advance();

    /// A boundary the queue has crossed: at queue frame `run_base` item
    /// `index` began, and it began at its own frame `item_base`.
    ///
    /// Recorded on the way past rather than computed from lengths, because the
    /// length of a track nobody has played yet is the decoder's business and
    /// frequently nobody's at all -- a stream has no length, and a VBR file's
    /// is a guess until it ends. What has already been played is not a guess.
    struct Mark {
        std::uint64_t run_base;
        std::size_t index;
        std::uint64_t item_base;
    };

    /// The mark covering queue frame `run`. Never empty once `open` succeeded.
    [[nodiscard]] std::size_t mark_for(std::uint64_t run) const noexcept;

    std::vector<Mark> marks_;

    IPlaylist* playlist_;
    ISource* current_ = nullptr;
    Format format_{};
    Format next_format_{};
    std::size_t index_ = 0;
    std::size_t completed_ = 0;
    std::uint64_t position_ = 0;
    QueueStop stopped_ = QueueStop::end;
    std::atomic<bool> skip_{false};
    bool done_ = false;
};

} // namespace mp

#endif // MEDIAPERCH_QUEUE_HPP

// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/queue.hpp"

#include <string>

namespace mp {

Queue::Queue(IPlaylist& playlist, std::size_t first) : playlist_(&playlist), index_(first)
{
}

bool Queue::open(std::string& why)
{
    const std::size_t count = playlist_->size();
    if (index_ >= count) {
        why = "the playlist has nothing at " + std::to_string(index_);
        return false;
    }
    const std::size_t from = index_;
    bool silent = false;
    current_ = first_from(index_, silent);
    if (current_ == nullptr) {
        if (silent) {
            // Not this queue's to play: an audio source cannot read a picture.
            // The host that holds the playlist plays it on the video engine's
            // clock, and asks for a queue again after it.
            why = "the entry at " + std::to_string(index_) +
                  " has no audio in it; its picture plays on the video engine's own clock";
            stopped_ = QueueStop::silent;
            return false;
        }
        // The playlist knows why, entry by entry; this is the count. A host
        // that holds the playlist puts its words here instead.
        index_ = from;
        why = count - from == 1
                  ? "the entry at " + std::to_string(from) + " would not open"
                  : "none of the " + std::to_string(count - from) + " entries from " +
                        std::to_string(from) + " would open";
        return false;
    }
    format_ = current_->format();
    if (!is_valid(format_)) {
        why = "the first item has no format";
        current_ = nullptr;
        return false;
    }
    position_ = 0;
    marks_.assign(1, Mark{0, index_, 0});
    stopped_ = QueueStop::end;
    done_ = false;
    return true;
}

bool Queue::jump(std::size_t index, std::uint64_t at)
{
    bool silent = false;
    ISource* item = first_from(index, silent);
    if (item == nullptr) {
        // "Next" on the last track is the end -- which is what it was when the
        // decoder was asked to skip instead -- and it is the end *here*, not
        // after the rest of the track has played. "Next" into a picture with
        // no audio is the end of this queue for the same reason a format
        // change is, and the host takes it from there.
        stopped_ = silent ? QueueStop::silent : QueueStop::end;
        done_ = true;
        position_ = at;
        return true;
    }
    // The one thing a queue may not do, exactly as `advance` refuses it: the
    // run ends saying which format the next track wants.
    if (item->format() != format_) {
        next_format_ = item->format();
        stopped_ = QueueStop::format_change;
        done_ = true;
        position_ = at;
        return true;
    }
    if (!item->seek(0)) {
        return false;
    }
    // Everything the decoder had recorded past this frame was read on a pass
    // this has just undone.
    while (!marks_.empty() && marks_.back().run_base >= at) {
        marks_.pop_back();
    }
    marks_.push_back(Mark{at, index, 0});
    ++completed_;
    index_ = index;
    current_ = item;
    position_ = at;
    done_ = false;
    skip_.store(false, std::memory_order_release);
    stopped_ = QueueStop::end;
    return true;
}

bool Queue::advance()
{
    std::size_t index = index_ + 1;
    bool silent = false;
    ISource* next = first_from(index, silent);
    if (next == nullptr) {
        stopped_ = silent ? QueueStop::silent : QueueStop::end;
        return false;
    }
    // **The one thing a queue may not do.** Two tracks of different formats
    // cannot share a device stream in exclusive mode, so joining them would
    // mean converting one of them without being asked. The queue stops, says
    // which format the next one wants, and leaves the decision where it
    // belongs.
    if (next->format() != format_) {
        next_format_ = next->format();
        stopped_ = QueueStop::format_change;
        return false;
    }
    index_ = index;
    current_ = next;
    // The boundary, now that it is behind us. Anything recorded at or after
    // this point was recorded on a pass that a seek has since undone.
    while (!marks_.empty() && marks_.back().run_base >= position_) {
        marks_.pop_back();
    }
    marks_.push_back(Mark{position_, index_, 0});
    return true;
}

ISource* Queue::first_from(std::size_t& index, bool& silent)
{
    // **An entry that will not open is walked past, not stopped at.** `at`
    // answers nullptr for one and for the end alike, and `size` is what tells
    // them apart. Nothing is silent about it: the playlist is what tried to
    // open the entry, and it says which and why.
    //
    // **A picture with no audio is stopped at, not walked past.** `at` answers
    // nullptr for that too -- there is no audio to hand out -- and `silent` is
    // what tells it from the other two. It is played, by the host, on the
    // video engine's own clock; this queue ends in front of it exactly as it
    // ends in front of another format, and another begins after it.
    silent = false;
    const std::size_t count = playlist_->size();
    for (; index < count; ++index) {
        if (ISource* item = playlist_->at(index)) {
            return item;
        }
        if (playlist_->silent(index)) {
            silent = true;
            return nullptr;
        }
    }
    return nullptr;
}

std::size_t Queue::read(void* dst, std::size_t bytes)
{
    if (current_ == nullptr || done_) {
        return 0;
    }
    const std::size_t stride = frame_bytes(format_);
    if (stride == 0) {
        return 0;
    }

    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t filled = 0;
    while (filled < bytes) {
        if (skip_.load(std::memory_order_acquire)) {
            skip_.store(false, std::memory_order_release);
            ++completed_;
            if (!advance()) {
                done_ = true;
                break;
            }
            continue;
        }

        const std::size_t got = current_->read(out + filled, bytes - filled);
        if (got != 0) {
            filled += got;
            position_ += got / stride;
            continue;
        }

        // **This is the whole of it.** The track ended; the buffer has not.
        // Opening the next one here rather than returning zero is what makes
        // the ring never notice, and the ring is what the device reads from.
        ++completed_;
        if (!advance()) {
            done_ = true;
            break;
        }
    }
    return filled;
}

bool Queue::seekable() const noexcept
{
    return current_ != nullptr && current_->seekable();
}

std::size_t Queue::mark_for(std::uint64_t run) const noexcept
{
    std::size_t m = marks_.size();
    while (m > 1 && marks_[m - 1].run_base > run) {
        --m;
    }
    return m == 0 ? 0 : m - 1;
}

std::size_t Queue::index_at(std::uint64_t run) const noexcept
{
    return marks_.empty() ? index_ : marks_[mark_for(run)].index;
}

std::uint64_t Queue::start_at(std::uint64_t run) const noexcept
{
    return marks_.empty() ? 0 : marks_[mark_for(run)].run_base;
}

bool Queue::has_previous_at(std::uint64_t run) const noexcept
{
    return !marks_.empty() && mark_for(run) > 0;
}

std::uint64_t Queue::previous_start_at(std::uint64_t run) const noexcept
{
    if (marks_.empty()) {
        return 0;
    }
    const std::size_t m = mark_for(run);
    return m > 0 ? marks_[m - 1].run_base : 0;
}

std::uint64_t Queue::item_start() const noexcept
{
    return marks_.empty() ? 0 : marks_[mark_for(position_)].run_base;
}

bool Queue::has_previous() const noexcept
{
    return mark_for(position_) > 0;
}

std::uint64_t Queue::previous_start() const noexcept
{
    const std::size_t m = mark_for(position_);
    return m > 0 ? marks_[m - 1].run_base : 0;
}

std::uint64_t Queue::item_position() const noexcept
{
    if (marks_.empty()) {
        return position_;
    }
    const Mark& mark = marks_[mark_for(position_)];
    return mark.item_base + (position_ - mark.run_base);
}

bool Queue::seek(std::uint64_t frame)
{
    if (marks_.empty()) {
        return false;
    }
    // Which track that queue frame is in, and where in it. A seek backwards
    // over a boundary reopens the track that was playing then -- the playlist
    // still has it, because a playlist that forgot what it had already handed
    // out could not be asked twice.
    const Mark mark = marks_[mark_for(frame)];
    // A "next" is a seek that lands on the track after this one, at its
    // start, from this frame. See `request_next`.
    if (next_wanted_.exchange(false, std::memory_order_acq_rel)) {
        return jump(mark.index + 1, frame);
    }
    ISource* item = playlist_->at(mark.index);
    if (item == nullptr || !item->seek(mark.item_base + (frame - mark.run_base))) {
        return false;
    }
    index_ = mark.index;
    current_ = item;
    position_ = frame;
    done_ = false;
    skip_.store(false, std::memory_order_release);
    stopped_ = QueueStop::end;
    return true;
}

std::uint64_t Queue::length_frames() const noexcept
{
    return current_ != nullptr ? current_->length_frames() : 0;
}

} // namespace mp

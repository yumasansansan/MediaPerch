// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/queue.hpp"

#include <string>

namespace mp {

Queue::Queue(IPlaylist& playlist, std::size_t first) : playlist_(&playlist), index_(first)
{
}

bool Queue::open(std::string& why)
{
    current_ = playlist_->at(index_);
    if (current_ == nullptr) {
        why = "the playlist has nothing at " + std::to_string(index_);
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

bool Queue::advance()
{
    ISource* next = playlist_->at(index_ + 1);
    if (next == nullptr) {
        stopped_ = QueueStop::end;
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
    ++index_;
    current_ = next;
    // The boundary, now that it is behind us. Anything recorded at or after
    // this point was recorded on a pass that a seek has since undone.
    while (!marks_.empty() && marks_.back().run_base >= position_) {
        marks_.pop_back();
    }
    marks_.push_back(Mark{position_, index_, 0});
    return true;
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

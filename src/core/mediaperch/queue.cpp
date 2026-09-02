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
    position_ = 0;
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
        if (skip_) {
            skip_ = false;
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

bool Queue::seek(std::uint64_t frame)
{
    if (current_ == nullptr || !current_->seek(frame)) {
        return false;
    }
    position_ = frame;
    done_ = false;
    stopped_ = QueueStop::end;
    return true;
}

std::uint64_t Queue::length_frames() const noexcept
{
    return current_ != nullptr ? current_->length_frames() : 0;
}

} // namespace mp

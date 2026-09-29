// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A demuxer, driven the way the host drives one, with what module.h promises
// about the answers held to it.
//
// probe, open, every stream described, every stream selected, packets read to
// the end or to a bound, seeks and more packets, close -- and on the way the
// calls a careless host could make on an object that exists: a null where a
// pointer should be, an index past the last stream, a buffer of no bytes.
// Shared by the demuxer fuzzers, for which a broken promise is a crash, and by
// the test that hands every demuxer broken copies of real files, for which it
// is a failure with the file's name; either way the driving stops there, since
// a module that wrote past a buffer has left nothing worth reading on.

#include <mediaperch/module.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp::test {

/// Bounds on the work one file may cause, so that a file claiming a billion
/// packets costs a moment rather than a timeout that means nothing.
struct DriveLimits {
    /// What a host would hand a packet at most. A container can claim
    /// anything; a demuxer that asks for more gets its packet refused, as the
    /// host refuses it, and the reading moves on.
    std::size_t largest_packet = std::size_t{64} << 20;
    int packets = 1024;
    int packets_after_seek = 32;
    std::uint32_t streams = 64;
    std::uint32_t largest_config = 1u << 20;
};

/// `broken(promise)` is called once, for the first promise broken, and the
/// driving stops. `chosen` picks the seeks: a fuzzer derives it from its input.
template <typename Broken>
class DemuxDrive {
public:
    DemuxDrive(const MpDemuxVtbl& v, const DriveLimits& limits, Broken& broken)
        : v_(v), limits_(limits), broken_(broken)
    {
    }

    void run(const char* path, const std::uint8_t* head, std::size_t head_bytes,
             std::uint64_t chosen)
    {
        // The probe sees what the host shows it: the first four kilobytes.
        const std::size_t shown = head_bytes < 4096 ? head_bytes : 4096;
        std::uint32_t score = 0;
        v_.probe(path, shown != 0 ? head : nullptr, shown, &score);
        if (score > 100 && !fail("a probe scores 0 to 100")) {
            return;
        }
        if (v_.probe(path, head, shown, nullptr) == MP_OK &&
            !fail("probe refuses a null out_score")) {
            return;
        }

        MpDemux* d = nullptr;
        if (v_.open(path, &d) != MP_OK) {
            return;
        }
        if (d == nullptr) {
            fail("open that succeeds hands back a demuxer");
            return;
        }
        d_ = d;
        read_all(chosen);
        v_.close(d);
        d_ = nullptr;
    }

private:
    const MpDemuxVtbl& v_;
    const DriveLimits& limits_;
    Broken& broken_;
    MpDemux* d_ = nullptr;
    bool failed_ = false;
    std::uint32_t streams_ = 0;
    std::vector<bool> selected_;
    std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(4096);

    /// Says the promise, and answers whether to go on: never.
    bool fail(const char* promise)
    {
        if (!failed_) {
            failed_ = true;
            broken_(promise);
        }
        return false;
    }

    void read_all(std::uint64_t chosen)
    {
        std::uint32_t count = 0;
        if (v_.stream_count(d_, &count) != MP_OK) {
            count = 0;
        }
        if (v_.stream_count(d_, nullptr) == MP_OK && !fail("stream_count refuses a null out")) {
            return;
        }
        streams_ = count < limits_.streams ? count : limits_.streams;
        if (!describe(count) || streams_ == 0) {
            return;
        }

        std::vector<std::uint32_t> every(streams_);
        for (std::uint32_t i = 0; i < streams_; ++i) {
            every[i] = i;
        }
        // Nonsense first, which must change nothing: no indices, none at all,
        // and one past the last stream.
        v_.select_streams(d_, nullptr, 1);
        v_.select_streams(d_, every.data(), 0);
        const std::uint32_t past = count;
        if (count != ~std::uint32_t{0} && v_.select_streams(d_, &past, 1) == MP_OK &&
            !fail("select_streams refuses an index past the last stream")) {
            return;
        }
        // Every stream, and if the demuxer serves one at a time, the first.
        selected_.assign(streams_, false);
        if (v_.select_streams(d_, every.data(), streams_) == MP_OK) {
            selected_.assign(streams_, true);
        } else if (v_.select_streams(d_, every.data(), 1) == MP_OK) {
            selected_[0] = true;
        }

        // A buffer of no bytes, and nowhere to say what was read.
        MpPacket nothing{};
        nothing.size = sizeof(MpPacket);
        if (v_.read_packet(d_, nullptr, 0, &nothing) == MP_OK && nothing.bytes != 0 &&
            !fail("read_packet writes nothing into a buffer of no bytes")) {
            return;
        }
        std::uint8_t one = 0;
        if (v_.read_packet(d_, &one, 1, nullptr) == MP_OK &&
            !fail("read_packet refuses a null out")) {
            return;
        }

        if (!read(limits_.packets)) {
            return;
        }
        // To the top, to wherever `chosen` says, and to the end of everything
        // -- each by a stream `chosen` names too, one past the last included.
        const std::uint64_t targets[] = {0, chosen, chosen >> 32, ~std::uint64_t{0}};
        for (std::size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            const auto stream = static_cast<std::uint32_t>((chosen >> (8 * t)) % (streams_ + 1u));
            v_.seek(d_, stream, targets[t]);
            if (!read(limits_.packets_after_seek)) {
                return;
            }
        }

        if (v_.read_frames != nullptr) {
            std::vector<std::uint8_t> frames(16384);
            std::size_t got = 0;
            if (v_.read_frames(d_, frames.data(), frames.size(), &got) == MP_OK &&
                got > frames.size()) {
                fail("read_frames writes no more than the buffer holds");
                return;
            }
            if (v_.read_frames(d_, frames.data(), frames.size(), nullptr) == MP_OK) {
                fail("read_frames refuses a null out_bytes");
            }
        }
    }

    /// The first streams, and the index one past the last of `count`.
    bool describe(std::uint32_t count)
    {
        // One past the last stream: an index a host miscounted is an answer,
        // not a read past the end of a table.
        MpStreamInfo past{};
        past.size = sizeof(MpStreamInfo);
        if (count != ~std::uint32_t{0} && v_.stream_info(d_, count, &past) == MP_OK) {
            return fail("stream_info refuses an index past the last stream");
        }
        for (std::uint32_t i = 0; i < streams_; ++i) {
            MpStreamInfo info{};
            info.size = sizeof(MpStreamInfo);
            v_.stream_info(d_, i, &info);
            if (v_.stream_info(d_, i, nullptr) == MP_OK) {
                return fail("stream_info refuses a null out");
            }
            // The two-call dance module.h describes: ask with no buffer -- one
            // of no bytes, which a blob that has any does not fit -- then with
            // one of the size it named.
            std::uint32_t needed = 0;
            const MpResult asked = v_.stream_config(d_, i, nullptr, 0, &needed);
            if (asked == MP_OK && needed != 0) {
                return fail("stream_config answers no buffer with MP_TOO_SMALL "
                            "when it needs bytes");
            }
            if (asked == MP_TOO_SMALL && needed != 0 && needed <= limits_.largest_config) {
                std::vector<std::uint8_t> config(needed);
                std::uint32_t again = 0;
                if (v_.stream_config(d_, i, config.data(), needed, &again) != MP_OK) {
                    return fail("stream_config fills a buffer of the size it named");
                }
            }
            if (v_.stream_config(d_, i, nullptr, 0, nullptr) == MP_OK) {
                return fail("stream_config refuses a null out_needed");
            }
            if (v_.size >= sizeof(MpDemuxVtbl) && v_.stream_video_info != nullptr) {
                MpVideoInfo video{};
                video.size = sizeof(MpVideoInfo);
                v_.stream_video_info(d_, i, &video);
                if (v_.stream_video_info(d_, i, nullptr) == MP_OK) {
                    return fail("stream_video_info refuses a null out");
                }
            }
        }
        return true;
    }

    /// Up to `packets` packets, the host's way: a packet that does not fit is
    /// asked for again in a buffer of the size it named.
    bool read(int packets)
    {
        for (int i = 0; i < packets; ++i) {
            MpPacket packet{};
            packet.size = sizeof(MpPacket);
            MpResult r = v_.read_packet(d_, buffer_.data(), buffer_.size(), &packet);
            if (r == MP_TOO_SMALL) {
                if (packet.bytes <= buffer_.size()) {
                    return fail("a packet that does not fit names more bytes than were offered");
                }
                if (packet.bytes > limits_.largest_packet) {
                    return true;
                }
                buffer_.resize(packet.bytes);
                packet = MpPacket{};
                packet.size = sizeof(MpPacket);
                r = v_.read_packet(d_, buffer_.data(), buffer_.size(), &packet);
            }
            if (r != MP_OK) {
                return true;
            }
            if (packet.bytes > buffer_.size()) {
                return fail("read_packet writes no more than the buffer holds");
            }
            if (packet.stream >= streams_ || !selected_[packet.stream]) {
                return fail("read_packet returns packets of selected streams only");
            }
        }
        return true;
    }
};

} // namespace mp::test

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MP4 container, given arbitrary bytes.
//
// **This replaced a fuzzer over a parser written here, and the replacement was
// not optional.** `modules/shared/mp4` used to read `moov`, and `alac_fuzzer`
// fed it the input as one; that parser is gone and `demux_mp4` reads MP4 with
// Bento4 instead. Deleting the target along with the parser would have traded
// five hundred fuzzed lines for eighty thousand unfuzzed ones -- so this walks
// the same path the module does, over the same library, and the tree's rule
// stands: **every parser that reads somebody else's file is fuzzed here.**
//
// It follows `demux_mp4` deliberately rather than calling the widest API it can
// find. Opening the file is the first parser; enumerating the tracks and their
// sample descriptions is the second; reading every sample through
// `AP4_LinearReader` is the third, and it is the one that walks `moof` chains on
// a fragmented file -- where Bento4's own recent history has a heap overflow in
// `AP4_BitReader::ReadCache` and a leak in `AP4_LinearReader::Tracker`.

#include "mp4_guard.hpp"

#include <Ap4.h>

#include <cstddef>
#include <cstdint>

namespace {

/// A read-only stream over the fuzzer's buffer. Nothing is copied: the input
/// outlives the run, and copying it would make the allocation the thing being
/// measured rather than the parser.
///
/// **Deliberately without the operation budget `demux_mp4`'s stream carries.**
/// The budget is a production defence -- it bounds a parse loop that has gone
/// wrong so a player does not stop responding. Putting it here too would make
/// the next unbounded loop in Bento4 look like a clean run, which is the
/// opposite of what a fuzzer is for. The guarded factory *is* shared, because
/// that changes which bytes get parsed at all and the two must not diverge.
class Bytes : public AP4_ByteStream {
public:
    Bytes(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    void AddReference() override { ++refs_; }
    void Release() override
    {
        if (--refs_ == 0) {
            delete this;
        }
    }

    AP4_Result ReadPartial(void* buffer, AP4_Size want, AP4_Size& got) override
    {
        const std::size_t left = at_ < size_ ? size_ - at_ : 0;
        got = static_cast<AP4_Size>(want < left ? want : left);
        if (got == 0) {
            return want == 0 ? AP4_SUCCESS : AP4_ERROR_EOS;
        }
        __builtin_memcpy(buffer, data_ + at_, got);
        at_ += got;
        return AP4_SUCCESS;
    }

    AP4_Result WritePartial(const void*, AP4_Size, AP4_Size& written) override
    {
        written = 0;
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_Result Seek(AP4_Position position) override
    {
        if (position > size_) {
            return AP4_ERROR_EOS;
        }
        at_ = static_cast<std::size_t>(position);
        return AP4_SUCCESS;
    }

    AP4_Result Tell(AP4_Position& position) override
    {
        position = at_;
        return AP4_SUCCESS;
    }

    AP4_Result GetSize(AP4_LargeSize& size) override
    {
        size = size_;
        return AP4_SUCCESS;
    }

protected:
    ~Bytes() override = default;

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
    int refs_ = 1;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    auto* stream = new Bytes(data, size);
    {
        // **The same factory the module installs**, so this fuzzes what ships.
        // Without it the corpus fails on its own regression seed, which is the
        // clearest possible statement of why the two must not diverge.
        mp::mp4::GuardedAtomFactory factory;
        AP4_File file(*stream, factory, false);
        AP4_Movie* movie = file.GetMovie();
        if (movie != nullptr) {
            AP4_LinearReader reader(*movie, stream);
            AP4_List<AP4_Track>& tracks = movie->GetTracks();
            for (AP4_List<AP4_Track>::Item* item = tracks.FirstItem(); item != nullptr;
                 item = item->GetNext()) {
                AP4_Track* track = item->GetData();
                if (track == nullptr) {
                    continue;
                }
                // The sample description is where the codec configuration comes
                // from, and where `demux_mp4` reaches into a `wave` box for a
                // QuickTime file.
                (void)track->GetSampleDescription(0);
                if (AP4_FAILED(reader.EnableTrack(track->GetId()))) {
                    continue;
                }

                // Bounded, because a crafted sample table can claim a very long
                // track out of very few bytes and a fuzzer that spends a minute
                // on one input is a fuzzer that finds nothing.
                AP4_Sample sample;
                AP4_DataBuffer sample_data;
                for (int i = 0; i < 256; ++i) {
                    if (AP4_FAILED(reader.ReadNextSample(track->GetId(), sample,
                                                         sample_data))) {
                        break;
                    }
                }
            }
        }
    }
    stream->Release();
    return 0;
}

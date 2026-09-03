// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPEG audio, given arbitrary bytes.
//
// **This used to fuzz dr_mp3 and now fuzzes libmpg123**, because that is what
// `demux_mpa` and `codec_mpa` are built on since the two were rewritten. The
// corpus did not change: `fuzz/corpus/mp3` is MPEG audio either way, and the
// input shape -- the whole buffer as a stream -- is what both parsers take.
//
// It walks the path the two modules walk, in the order they walk it: parse the
// stream frame by frame without decoding, which is `demux_mpa`, then feed the
// same bytes to a decoder, which is `codec_mpa`. Both halves are libmpg123 and
// both are reachable from a file, so both belong here.
//
// MPEG audio earns a fuzzer more than most formats in this tree, because its
// frame parser resynchronises after garbage and will keep reading through a
// file that is mostly not MPEG audio at all.

#include <mpg123.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

/// The fuzzer's buffer, as a stream. mpg123's feed interface would do, but the
/// reader interface is what `demux_mpa` installs, so it is what gets tested.
struct Cursor {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at;
};

int io_read(void* handle, void* buffer, std::size_t count, std::size_t* got)
{
    auto* c = static_cast<Cursor*>(handle);
    const std::size_t left = c->at < c->size ? c->size - c->at : 0;
    const std::size_t n = count < left ? count : left;
    if (n != 0) {
        std::memcpy(buffer, c->data + c->at, n);
        c->at += n;
    }
    if (got != nullptr) {
        *got = n;
    }
    return 0;
}

std::int64_t io_seek(void* handle, std::int64_t offset, int whence)
{
    auto* c = static_cast<Cursor*>(handle);
    std::int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<std::int64_t>(c->at);
    } else if (whence == SEEK_END) {
        base = static_cast<std::int64_t>(c->size);
    }
    const std::int64_t to = base + offset;
    if (to < 0 || to > static_cast<std::int64_t>(c->size)) {
        return -1;
    }
    c->at = static_cast<std::size_t>(to);
    return to;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 4) {
        return 0;
    }

    // The container half: parse frames, take their bytes, never decode.
    Cursor cursor{data, size, 0};
    int err = MPG123_OK;
    mpg123_handle* mh = mpg123_new(nullptr, &err);
    if (mh == nullptr) {
        return 0;
    }
    // The same flags the modules set, so this fuzzes what ships. ID3v2 in
    // particular: `store_id3v2` allocates a tag's declared length before
    // reading it, which is how this target first reached 380 MB on a
    // few-hundred-byte input. The modules skip tags because they read none.
    mpg123_param2(mh, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_SKIP_ID3V2, 0.0);
    mpg123_param2(mh, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0.0);
    if (mpg123_reader64(mh, io_read, io_seek, nullptr) == MPG123_OK &&
        mpg123_open_handle64(mh, &cursor) == MPG123_OK) {
        (void)mpg123_scan(mh);
        (void)mpg123_length64(mh);
        long value = 0;
        double fvalue = 0.0;
        (void)mpg123_getstate2(mh, MPG123_ENC_DELAY, &value, &fvalue);
        (void)mpg123_getstate2(mh, MPG123_ENC_PADDING, &value, &fvalue);

        // Bounded: a crafted header can claim a very long stream out of very few
        // bytes, and a fuzzer that spends a minute on one input finds nothing.
        for (int i = 0; i < 512; ++i) {
            if (mpg123_framebyframe_next(mh) < 0) {
                break;
            }
            unsigned long header = 0;
            unsigned char* body = nullptr;
            std::size_t body_bytes = 0;
            if (mpg123_framedata(mh, &header, &body, &body_bytes) != MPG123_OK) {
                break;
            }
        }
        // The seek index, which is the other thing `mpg123_scan` built.
        (void)mpg123_seek_frame64(mh, 1, SEEK_SET);
        (void)mpg123_framebyframe_next(mh);
    }
    mpg123_close(mh);
    mpg123_delete(mh);

    // The codec half: the same bytes fed to a decoder, one shot, float out.
    mpg123_handle* dec = mpg123_new(nullptr, &err);
    if (dec == nullptr) {
        return 0;
    }
    mpg123_param2(dec, MPG123_ADD_FLAGS,
                  MPG123_QUIET | MPG123_FORCE_FLOAT | MPG123_SKIP_ID3V2, 0.0);
    mpg123_param2(dec, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0.0);
    if (mpg123_open_feed(dec) == MPG123_OK &&
        mpg123_feed(dec, data, size) == MPG123_OK) {
        for (int i = 0; i < 512; ++i) {
            std::int64_t number = 0;
            unsigned char* audio = nullptr;
            std::size_t got = 0;
            const int status = mpg123_decode_frame64(dec, &number, &audio, &got);
            if (status == MPG123_NEED_MORE || status == MPG123_DONE) {
                break;
            }
            if (status != MPG123_OK && status != MPG123_NEW_FORMAT) {
                break;
            }
        }
    }
    mpg123_close(dec);
    mpg123_delete(dec);
    return 0;
}

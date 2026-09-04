// SPDX-License-Identifier: GPL-3.0-or-later
//
// libwavpack, given arbitrary bytes.
//
// **libwavpack is on OSS-Fuzz already**, which is a reason to expect this to
// find nothing and not a reason to leave it out. The targets here exist for
// two things besides finding bugs: somewhere for a regression corpus to live,
// and a check that the machinery still builds -- and the machinery is the part
// that rots. libFLAC and libmpg123 are fuzzed upstream too and have targets
// here for the same reason.
//
// The whole buffer is a file, read through the same `WavpackStreamReader64`
// callbacks `demux_wavpack` installs, so what is exercised is the path this
// tree actually takes: the header, the block walk, the decorrelation, and
// `OPEN_DSD_NATIVE`, which is the one this module needs and which a corpus of
// PCM files would never reach.

#include <wavpack.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct Cursor {
    const std::uint8_t* data;
    std::int64_t size;
    std::int64_t at;
    int pushed;
};

std::int32_t io_read(void* id, void* data, std::int32_t bytes)
{
    auto* c = static_cast<Cursor*>(id);
    if (bytes <= 0) {
        return 0;
    }
    auto* out = static_cast<std::uint8_t*>(data);
    std::int32_t got = 0;
    if (c->pushed >= 0) {
        *out++ = static_cast<std::uint8_t>(c->pushed);
        c->pushed = -1;
        got = 1;
        if (bytes == 1) {
            return 1;
        }
    }
    const std::int64_t left = c->at < c->size ? c->size - c->at : 0;
    const std::int64_t n = (bytes - got) < left ? (bytes - got) : left;
    if (n > 0) {
        std::memcpy(out, c->data + c->at, static_cast<std::size_t>(n));
        c->at += n;
    }
    return got + static_cast<std::int32_t>(n);
}

std::int32_t io_write(void*, void*, std::int32_t) { return 0; }

std::int64_t io_get_pos(void* id)
{
    auto* c = static_cast<Cursor*>(id);
    return c->pushed >= 0 ? c->at - 1 : c->at;
}

int io_set_pos_abs(void* id, std::int64_t pos)
{
    auto* c = static_cast<Cursor*>(id);
    c->pushed = -1;
    if (pos < 0 || pos > c->size) {
        return -1;
    }
    c->at = pos;
    return 0;
}

int io_set_pos_rel(void* id, std::int64_t delta, int mode)
{
    auto* c = static_cast<Cursor*>(id);
    std::int64_t base = 0;
    if (mode == SEEK_CUR) {
        base = c->pushed >= 0 ? c->at - 1 : c->at;
    } else if (mode == SEEK_END) {
        base = c->size;
    }
    return io_set_pos_abs(id, base + delta);
}

int io_push_back(void* id, int ch)
{
    static_cast<Cursor*>(id)->pushed = ch;
    return ch;
}

std::int64_t io_get_length(void* id) { return static_cast<Cursor*>(id)->size; }
int io_can_seek(void*) { return 1; }
int io_truncate(void*) { return -1; }
int io_close(void*) { return 0; }

WavpackStreamReader64 g_reader = {
    &io_read,      &io_write,      &io_get_pos,  &io_set_pos_abs, &io_set_pos_rel,
    &io_push_back, &io_get_length, &io_can_seek, &io_truncate,    &io_close,
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 32 || size > (8u << 20)) {
        return 0;
    }
    Cursor cursor{data, static_cast<std::int64_t>(size), 0, -1};
    char why[80] = {};
    WavpackContext* wpc = WavpackOpenFileInputEx64(&g_reader, &cursor, nullptr, why,
                                                   OPEN_DSD_NATIVE | OPEN_ALT_TYPES, 0);
    if (wpc == nullptr) {
        return 0;
    }
    const int channels = WavpackGetNumChannels(wpc);
    if (channels > 0 && channels <= 8) {
        // Bounded: a header can claim a very long stream out of very few bytes,
        // and a fuzzer that spends a minute on one input finds nothing.
        constexpr std::uint32_t k_frames = 1024;
        std::vector<std::int32_t> out(static_cast<std::size_t>(k_frames) * channels);
        for (int i = 0; i < 16; ++i) {
            if (WavpackUnpackSamples(wpc, out.data(), k_frames) == 0) {
                break;
            }
        }
        // And from the middle, which is the path the block index takes.
        if (WavpackSeekSample64(wpc, 4096) != 0) {
            WavpackUnpackSamples(wpc, out.data(), k_frames);
        }
    }
    WavpackCloseFile(wpc);
    return 0;
}

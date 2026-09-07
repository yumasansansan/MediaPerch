// SPDX-License-Identifier: GPL-3.0-or-later
//
// One `MpDspVtbl` made of a counter, so a chain can be assembled in a test.
//
// **It is not a signal processor and does not pretend to be one.** What
// `dsp_test.cpp` checks is the chain's arithmetic against a stage that does
// real work; what this is for is everything *about* a stage that is not its
// samples -- that it appears as a node, that its settings can be read and
// written, that the order it was put in is the order it runs. A stage that
// multiplied would make those tests about multiplication.

#ifndef MEDIAPERCH_TESTS_FAKE_DSP_HPP
#define MEDIAPERCH_TESTS_FAKE_DSP_HPP

#include <mediaperch/module.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace mp::test {
namespace detail {

/// One instance's only state. A pointer to this is the handle, which is what
/// makes two stages in one chain two things rather than one.
struct FakeDsp {
    double amount = 1.0;
    MpFormat format{};
};

inline MpResult MP_CALL dsp_open(MpDsp** out) noexcept
{
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = reinterpret_cast<MpDsp*>(new (std::nothrow) FakeDsp());
    return *out != nullptr ? MP_OK : MP_ERR_NO_MEMORY;
}

inline void MP_CALL dsp_close(MpDsp* d) noexcept
{
    delete reinterpret_cast<FakeDsp*>(d);
}

inline MpResult MP_CALL dsp_configure(MpDsp* d, const MpFormat* in, std::uint32_t,
                                      MpFormat* out, std::uint32_t* out_max) noexcept
{
    if (d == nullptr || in == nullptr || out == nullptr || out_max == nullptr) {
        return MP_ERR_INVALID;
    }
    reinterpret_cast<FakeDsp*>(d)->format = *in;
    *out = *in;
    *out_max = 0; // no more frames out than in
    return MP_OK;
}

inline MpResult MP_CALL dsp_process(MpDsp* d, const double* const* in,
                                    std::uint32_t in_frames, double* const* out,
                                    std::uint32_t, std::uint32_t* out_frames) noexcept
{
    if (d == nullptr || out_frames == nullptr) {
        return MP_ERR_INVALID;
    }
    const auto* self = reinterpret_cast<const FakeDsp*>(d);
    const std::uint32_t channels = self->format.channels;
    for (std::uint32_t c = 0; c < channels; ++c) {
        for (std::uint32_t f = 0; f < in_frames; ++f) {
            out[c][f] = in[c][f] * self->amount;
        }
    }
    *out_frames = in_frames;
    return MP_OK;
}

inline MpResult MP_CALL dsp_flush(MpDsp*, double* const*, std::uint32_t,
                                  std::uint32_t* out_frames) noexcept
{
    if (out_frames != nullptr) {
        *out_frames = 0;
    }
    return MP_OK;
}

inline MpResult MP_CALL dsp_reset(MpDsp*) noexcept { return MP_OK; }

inline MpResult MP_CALL dsp_latency(MpDsp*, std::uint32_t* out_frames) noexcept
{
    if (out_frames != nullptr) {
        *out_frames = 0;
    }
    return MP_OK;
}

inline MpResult MP_CALL dsp_set(MpDsp* d, const char* key, const char* value) noexcept
{
    if (d == nullptr || key == nullptr || value == nullptr) {
        return MP_ERR_INVALID;
    }
    if (std::strcmp(key, "amount") != 0) {
        // **Named, so a caller's refusal path is a path something takes.** A
        // canvas that could set a key a stage does not have would be a canvas
        // showing a chain the engine does not have.
        return MP_ERR_UNSUPPORTED;
    }
    char* end = nullptr;
    const double amount = std::strtod(value, &end);
    if (end == value || *end != '\0') {
        return MP_ERR_INVALID;
    }
    reinterpret_cast<FakeDsp*>(d)->amount = amount;
    return MP_OK;
}

inline MpResult MP_CALL dsp_describe(MpDsp* d, std::uint32_t index, char* out,
                                     std::uint32_t out_bytes) noexcept
{
    if (d == nullptr || out == nullptr || out_bytes < 32) {
        return MP_ERR_INVALID;
    }
    if (index == 0) {
        std::snprintf(out, out_bytes, "amount\t%g\twhat every sample is multiplied by",
                      reinterpret_cast<const FakeDsp*>(d)->amount);
        return MP_OK;
    }
    // **A measurement, spelled the way every module in this tree spells one.**
    // A stage answers with things it will not take back -- a peak, a cost, a
    // latency -- and marks them by ending the description with `(read only)`.
    // Something has to read that, and it is the engine rather than each shell;
    // this row is what makes that checkable without a real module.
    if (index == 1) {
        std::snprintf(out, out_bytes, "peak\t%.6f\tloudest sample seen (read only)",
                      reinterpret_cast<const FakeDsp*>(d)->amount);
        return MP_OK;
    }
    return MP_END;
}

} // namespace detail

inline const MpDspVtbl& fake_dsp_vtbl()
{
    static const MpDspVtbl vtbl{sizeof(MpDspVtbl),
                                0,
                                &detail::dsp_open,
                                &detail::dsp_close,
                                &detail::dsp_configure,
                                &detail::dsp_process,
                                &detail::dsp_flush,
                                &detail::dsp_set,
                                &detail::dsp_describe,
                                &detail::dsp_reset,
                                &detail::dsp_latency};
    return vtbl;
}

} // namespace mp::test

#endif // MEDIAPERCH_TESTS_FAKE_DSP_HPP

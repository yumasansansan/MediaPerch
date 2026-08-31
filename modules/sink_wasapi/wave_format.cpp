// SPDX-License-Identifier: GPL-3.0-or-later
#include "wave_format.hpp"

#include <cstring>

namespace mp::wasapi {
namespace {

UINT32 container_bytes(MpSampleType type) noexcept
{
    switch (type) {
    case MP_SAMPLE_S16: return 2;
    case MP_SAMPLE_S24_PACKED: return 3;
    case MP_SAMPLE_S24_IN_32: return 4;
    case MP_SAMPLE_S32: return 4;
    case MP_SAMPLE_F32: return 4;
    default: return 0;
    }
}

UINT32 natural_valid_bits(MpSampleType type) noexcept
{
    switch (type) {
    case MP_SAMPLE_S16: return 16;
    case MP_SAMPLE_S24_PACKED: return 24;
    case MP_SAMPLE_S24_IN_32: return 24;
    case MP_SAMPLE_S32: return 32;
    case MP_SAMPLE_F32: return 32;
    default: return 0;
    }
}

} // namespace

UINT32 frame_bytes_of(const MpFormat& format) noexcept
{
    return container_bytes(format.sample_type) * format.channels;
}

bool to_wave_format(const MpFormat& format, WAVEFORMATEXTENSIBLE& out) noexcept
{
    // A bitstream needs a subformat GUID per codec and there is nothing in the
    // tree that produces one yet. Refusing is honest; guessing would not be.
    if (format.encoding == MP_ENCODING_IEC61937) {
        return false;
    }

    const UINT32 bytes = container_bytes(format.sample_type);
    if (bytes == 0 || format.channels == 0 || format.sample_rate == 0) {
        return false;
    }

    const UINT32 valid =
        format.valid_bits != 0 ? format.valid_bits : natural_valid_bits(format.sample_type);
    if (valid > bytes * 8) {
        return false;
    }

    std::memset(&out, 0, sizeof(out));
    WAVEFORMATEX& base = out.Format;
    base.nChannels = static_cast<WORD>(format.channels);
    base.nSamplesPerSec = format.sample_rate;
    base.wBitsPerSample = static_cast<WORD>(bytes * 8);
    base.nBlockAlign = static_cast<WORD>(bytes * format.channels);
    base.nAvgBytesPerSec = base.nSamplesPerSec * base.nBlockAlign;

    const bool needs_extensible = format.channel_mask != 0 || format.channels > 2 ||
                                  valid != base.wBitsPerSample;

    if (!needs_extensible) {
        base.wFormatTag = format.sample_type == MP_SAMPLE_F32 ? WAVE_FORMAT_IEEE_FLOAT
                                                              : WAVE_FORMAT_PCM;
        base.cbSize = 0;
        return true;
    }

    base.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    base.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    out.Samples.wValidBitsPerSample = static_cast<WORD>(valid);
    out.dwChannelMask = format.channel_mask;
    out.SubFormat =
        format.sample_type == MP_SAMPLE_F32 ? subtype_ieee_float : subtype_pcm;
    return true;
}

bool from_wave_format(const WAVEFORMATEX& wfx, MpFormat& out) noexcept
{
    std::memset(&out, 0, sizeof(out));
    out.sample_rate = wfx.nSamplesPerSec;
    out.channels = wfx.nChannels;
    out.encoding = MP_ENCODING_PCM;

    bool is_float = wfx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    UINT32 valid = wfx.wBitsPerSample;

    if (wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wfx);
        out.channel_mask = ext.dwChannelMask;
        valid = ext.Samples.wValidBitsPerSample;
        is_float = std::memcmp(&ext.SubFormat, &subtype_ieee_float, sizeof(GUID)) == 0;
    }

    if (is_float) {
        if (wfx.wBitsPerSample != 32) {
            return false;
        }
        out.sample_type = MP_SAMPLE_F32;
        out.valid_bits = 0;
        return true;
    }

    switch (wfx.wBitsPerSample) {
    case 16:
        out.sample_type = MP_SAMPLE_S16;
        break;
    case 24:
        out.sample_type = MP_SAMPLE_S24_PACKED;
        break;
    case 32:
        // The distinction the whole widening scheme rests on: 24 valid bits in a
        // four-byte container is not the same format as 32 valid bits, and the
        // only thing that tells them apart is wValidBitsPerSample.
        out.sample_type = valid == 24 ? MP_SAMPLE_S24_IN_32 : MP_SAMPLE_S32;
        break;
    default:
        return false;
    }

    out.valid_bits = valid == natural_valid_bits(out.sample_type) ? 0 : valid;
    return true;
}

} // namespace mp::wasapi

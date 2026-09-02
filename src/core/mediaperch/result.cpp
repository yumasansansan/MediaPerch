// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/result.hpp"

#include <cstdio>

namespace mp {

const char* result_name(MpResult r) noexcept
{
    switch (r) {
    case MP_OK: return "ok";
    case MP_END: return "end";
    case MP_ERR_INVALID: return "invalid";
    case MP_ERR_UNSUPPORTED: return "unsupported by this module";
    case MP_ERR_FORMAT: return "format refused";
    case MP_ERR_IO: return "io";
    case MP_ERR_DEVICE_LOST: return "device lost";
    case MP_ERR_BUSY: return "device in use by another application";
    case MP_ERR_DENIED: return "exclusive mode is disabled for this device";
    case MP_ERR_NO_MEMORY: return "out of memory";
    case MP_TIMEOUT: return "timed out";
    default: return "internal error";
    }
}

const char* codec_name(MpCodec codec) noexcept
{
    switch (codec) {
    case MP_CODEC_PCM: return "PCM";
    case MP_CODEC_DSD: return "DSD";
    case MP_CODEC_FLAC: return "FLAC";
    case MP_CODEC_ALAC: return "ALAC";
    case MP_CODEC_WAVPACK: return "WavPack";
    case MP_CODEC_APE: return "Monkey's Audio";
    case MP_CODEC_TTA: return "TTA";
    case MP_CODEC_MP1: return "MPEG-1 layer I";
    case MP_CODEC_MP2: return "MPEG-1 layer II";
    case MP_CODEC_MP3: return "MP3";
    case MP_CODEC_AAC_LC: return "AAC-LC";
    case MP_CODEC_HE_AAC: return "HE-AAC";
    case MP_CODEC_VORBIS: return "Vorbis";
    case MP_CODEC_OPUS: return "Opus";
    case MP_CODEC_SPEEX: return "Speex";
    case MP_CODEC_WMA: return "WMA";
    case MP_CODEC_AC3: return "AC-3";
    case MP_CODEC_EAC3: return "E-AC-3";
    case MP_CODEC_DTS: return "DTS";
    // Not "unknown": the module has told us it decodes this itself, which is a
    // statement, and printing it as an absence would misread the flag.
    case MP_CODEC_INTERNAL: return "internal";
    case MP_CODEC_UNKNOWN: return "unknown";
    default: break;
    }
    // A codec id from a module newer than this build. Static, because callers
    // print it and nothing here owns a string.
    static thread_local char buffer[24];
    std::snprintf(buffer, sizeof buffer, "codec 0x%08x", static_cast<unsigned>(codec));
    return buffer;
}

const char* stream_kind_name(MpStreamKind kind) noexcept
{
    switch (kind) {
    case MP_STREAM_AUDIO: return "audio";
    case MP_STREAM_VIDEO: return "video";
    case MP_STREAM_SUBTITLE: return "subtitle";
    default: return "other";
    }
}

const char* module_kind_name(MpKind kind) noexcept
{
    switch (kind) {
    case MP_KIND_SINK: return "sink";
    case MP_KIND_DSP: return "dsp";
    case MP_KIND_VIDEO: return "video";
    case MP_KIND_META: return "meta";
    case MP_KIND_DEMUX: return "demux";
    case MP_KIND_CODEC: return "codec";
    default: return "unknown";
    }
}

} // namespace mp

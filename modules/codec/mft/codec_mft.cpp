// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video decoding through one Media Foundation transform, and not through MF.
//
// **The distinction is the whole of plan.md §9.8.** `IMFSourceReader` is a
// pipeline with no seam in it -- it opens the file, demuxes it, decodes it, and
// there is no point at which you can ask where a frame came from. That is what
// `decode_mf` was under ABI v1 and it is why v1 could not say. `IMFTransform`
// is one decoder: a bitstream in, frames out, no file, no container, no seeking.
// This module is the second, so the container stays `demux_mp4` -- fuzzed, with
// seeking measured byte-identically across four framings -- and the black box
// shrinks to one function whose output can be held against another decoder's.
//
// **It is a D3D11 module, and that is a fact about Media Foundation.**
// `IMFDXGIDeviceManager` wraps an `ID3D11Device` and there is no D3D12
// equivalent for an MFT; D3D12 video decoding is `ID3D12VideoDevice` and a
// different command list entirely. So `probe` scores 0 for MP_GRAPHICS_D3D12,
// a D3D12 decoder is a sibling module rather than a flag in this one, and a
// host picks the decoder that matches the presenter's device -- which is what
// the `api` argument to `probe` is for.
//
// Worth naming precisely, because the names are three generations deep and get
// used interchangeably. **DXVA2 is a Direct3D 9 API** -- `IDirectXVideoDecoder`
// and its service -- and this module does not touch it. The D3D11 equivalent is
// `ID3D11VideoDevice`, which FFmpeg calls `d3d11va`, and D3D12's is
// `ID3D12VideoDevice`. What all three share is the DXVA *specification*: the
// decoder GUIDs, the bitstream buffer layout, the picture parameter structures.
// The protocol is DXVA; the API to reach it is whichever Direct3D you are on.
// An MFT sits above all of that and hands back a texture.
//
// **Two output paths, and the software one is not a consolation.** With a
// device, a hardware MFT decodes into a texture on it and nothing crosses the
// bus. Without one -- a machine with no usable adapter, a CI runner, or a host
// that asked for system memory -- Microsoft's software H.264 decoder produces
// NV12 in memory. The second is deterministic, which is what makes a decoded
// frame something a test can hash rather than look at.

#include "avcc.hpp"

#include <mediaperch/module.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

namespace {

const MpHost* g_host = nullptr;

void log_line(MpLogLevel level, const char* msg) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, msg);
    }
}

/// The same small COM pointer the presenter has, for the same reason: this file
/// holds eight of them and the alternative is eight releases in the right order.
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    T** put()
    {
        reset();
        return &p_;
    }
    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept
    {
        if (p_ != nullptr) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

/// `MFStartup` once for the module, and `MFShutdown` when it unloads. Both are
/// reference counted by MF itself, but calling them per decoder would mean a
/// playlist starting and stopping Media Foundation between tracks.
class Runtime {
public:
    Runtime() { ok_ = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~Runtime()
    {
        if (ok_) {
            ::MFShutdown();
        }
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

Runtime& runtime()
{
    static Runtime r;
    return r;
}

/// The MF subtype for a codec this module claims, or GUID_NULL.
GUID subtype_of(MpCodec codec) noexcept
{
    switch (codec) {
    case MP_CODEC_H264:
        return MFVideoFormat_H264;
    case MP_CODEC_HEVC:
        return MFVideoFormat_HEVC;
    default:
        break;
    }
    return GUID_NULL;
}

} // namespace

struct MpVideoCodec {
    Com<IMFTransform> transform;
    Com<IMFDXGIDeviceManager> manager;
    Com<ID3D11Device> device;

    mp::mft::AvcConfig avcc;
    MpCodec codec = MP_CODEC_UNKNOWN;
    /// True when frames come back as textures rather than as planes.
    bool on_gpu = false;
    bool streaming = false;
    /// The parameter sets go in front of the next packet, which is true at the
    /// start and again after every reset.
    bool needs_parameter_sets = true;

    MpVideoInfo info{};
    bool have_format = false;

    /// The sample being handed out, and whatever it is holding, kept alive
    /// until the next call because that is what `next_frame` promises.
    Com<IMFSample> out_sample;
    Com<IMFMediaBuffer> out_buffer;
    Com<IMF2DBuffer> out_2d;
    Com<ID3D11Texture2D> out_texture;
    /// **Three ways to get at a buffer's rows, and a decoder implements one.**
    /// `IMF2DBuffer2` is the newest and the software H.264 transform does not
    /// have it; `IMF2DBuffer` is the older one that states a pitch; a plain
    /// `IMFMediaBuffer` states none and the pitch has to come from the media
    /// type. Which was held has to be remembered, because they unlock
    /// differently.
    enum class Lock { none, two_d, flat } out_lock = Lock::none;
    std::uint8_t* out_scanline0 = nullptr;
    LONG out_pitch = 0;

    /// `MF_MT_DEFAULT_STRIDE`, for the buffer that will not say.
    LONG default_stride = 0;

    /// One packet, rewritten as Annex B. Reused so that decoding does not
    /// allocate per frame.
    std::vector<std::uint8_t> annex_b;

    std::string trouble;
};

namespace {

/// Lets go of whatever the last `next_frame` handed out.
void release_frame(MpVideoCodec* c) noexcept
{
    if (c->out_lock == MpVideoCodec::Lock::two_d && c->out_2d) {
        c->out_2d->Unlock2D();
    } else if (c->out_lock == MpVideoCodec::Lock::flat && c->out_buffer) {
        c->out_buffer->Unlock();
    }
    c->out_lock = MpVideoCodec::Lock::none;
    c->out_scanline0 = nullptr;
    c->out_pitch = 0;
    c->out_texture.reset();
    c->out_2d.reset();
    c->out_buffer.reset();
    c->out_sample.reset();
}

/// The output type the transform settled on, which is where the geometry comes
/// from -- a decoder learns it from the bitstream, not from the container.
bool read_output_format(MpVideoCodec* c)
{
    Com<IMFMediaType> type;
    if (FAILED(c->transform->GetOutputCurrentType(0, type.put()))) {
        return false;
    }
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(::MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height))) {
        return false;
    }
    GUID subtype = GUID_NULL;
    (void)type->GetGUID(MF_MT_SUBTYPE, &subtype);

    const std::uint32_t size = c->info.size != 0 ? c->info.size : sizeof(MpVideoInfo);
    c->info = MpVideoInfo{};
    c->info.size = size;
    c->info.width = width;
    c->info.height = height;
    c->info.display_width = width;
    c->info.display_height = height;

    UINT32 num = 0;
    UINT32 den = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &num, &den))) {
        c->info.fps_num = num;
        c->info.fps_den = den;
    }

    // **The container's colour wins over the decoder's**, and this module does
    // not have the container's. `demux_mp4` states primaries, transfer and
    // matrix from `colr`; what MF reports is what it guessed from the
    // bitstream, which is the same guess with less information. So these stay
    // unspecified and the host takes the demuxer's answer -- see §9.9.1, where
    // the difference between the two decides whether a frame is tone-mapped.
    c->info.primaries = 2;
    c->info.transfer = 2;
    c->info.matrix = 2;

    // Ticks are the presentation timestamps MF hands back, which are hundred
    // nanosecond units. Stated rather than assumed, because §9.9 is about
    // exactly this not being stated.
    c->info.timescale = 10000000u;

    // For a buffer that cannot state its own pitch. A negative value here is
    // MF's way of saying bottom-up, which a decoder's NV12 output never is.
    UINT32 stride = 0;
    c->default_stride =
        SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))
            ? static_cast<LONG>(static_cast<INT32>(stride))
            : static_cast<LONG>(width);
    c->have_format = true;
    (void)subtype;
    return true;
}

bool set_input_type(MpVideoCodec* c, std::string& why)
{
    Com<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.put()))) {
        why = "no media type";
        return false;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype_of(c->codec));
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
    if (FAILED(c->transform->SetInputType(0, type.get(), 0))) {
        why = "the decoder would not take the stream";
        return false;
    }
    return true;
}

/// The first uncompressed type the transform offers that this tree can present.
bool set_output_type(MpVideoCodec* c, std::string& why)
{
    for (DWORD i = 0;; ++i) {
        Com<IMFMediaType> type;
        const HRESULT hr = c->transform->GetOutputAvailableType(0, i, type.put());
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }
        GUID subtype = GUID_NULL;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }
        // NV12 is what every hardware decoder produces for 8-bit and P010 for
        // 10-bit. Anything else -- and MF offers YUY2 and IYUV as well -- is a
        // format this tree has nowhere to put, so it is passed over rather
        // than accepted and converted somewhere invisible.
        if (subtype != MFVideoFormat_NV12 && subtype != MFVideoFormat_P010) {
            continue;
        }
        if (SUCCEEDED(c->transform->SetOutputType(0, type.get(), 0))) {
            return true;
        }
    }
    why = "the decoder offers no NV12 or P010 output";
    return false;
}

/// Finds a transform for `codec`, preferring a hardware one when there is a
/// device to bind it to.
bool activate(MpVideoCodec* c, bool want_hardware, std::string& why)
{
    MFT_REGISTER_TYPE_INFO input{};
    input.guidMajorType = MFMediaType_Video;
    input.guidSubtype = subtype_of(c->codec);

    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
    flags |= want_hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                              MFT_ENUM_FLAG_SYNCMFT)
                           : MFT_ENUM_FLAG_SYNCMFT;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input, nullptr, &activates,
                           &count)) ||
        count == 0) {
        why = want_hardware ? "no hardware decoder for this codec"
                            : "no decoder for this codec at all";
        if (activates != nullptr) {
            ::CoTaskMemFree(activates);
        }
        return false;
    }

    bool made = false;
    bool passed_over_async = false;
    for (UINT32 i = 0; i < count; ++i) {
        // **An asynchronous MFT is a different program, not a faster one.**
        // `MFTEnumEx` marks one with `MF_TRANSFORM_ASYNC`; it has to be
        // unlocked with `MF_TRANSFORM_ASYNC_UNLOCK` and then driven by
        // `METransformNeedInput` and `METransformHaveOutput` events, and until
        // it is, every `IMFTransform` method returns
        // MF_E_TRANSFORM_ASYNC_LOCKED. Nothing here drives an event queue yet,
        // and activating one anyway would fail at `SetInputType` with the
        // software fallback already spent -- so it is passed over, which leaves
        // the synchronous decoder for this machine to find.
        //
        // That is not a hypothetical loss: several vendors ship async hardware
        // decoders. It is a smaller one than it sounds, because Microsoft's own
        // H.264 transform is synchronous *and* D3D11-aware, so it is the DXVA
        // host on an Intel machine rather than a software fallback.
        UINT32 async = 0;
        if (SUCCEEDED(activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0) {
            passed_over_async = true;
            activates[i]->Release();
            continue;
        }
        if (!made && SUCCEEDED(activates[i]->ActivateObject(
                         IID_PPV_ARGS(c->transform.put())))) {
            made = true;
        }
        activates[i]->Release();
    }
    ::CoTaskMemFree(activates);
    if (!made) {
        why = passed_over_async
                  ? "every decoder for this codec is an asynchronous MFT, which nothing "
                    "here drives yet"
                  : "the decoder would not activate";
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

MpResult MP_CALL codec_probe(MpCodec codec, MpGraphicsApi api, const std::uint8_t* config,
                             std::uint32_t config_bytes, std::uint32_t* out_score) noexcept
{
    if (out_score == nullptr) {
        return MP_ERR_INVALID;
    }
    *out_score = 0;
    if (subtype_of(codec) == GUID_NULL) {
        return MP_OK;
    }
    // **Media Foundation is a D3D11 library.** `IMFDXGIDeviceManager` takes an
    // ID3D11Device and there is no D3D12 form of it, so a host with a D3D12
    // presenter must find a D3D12 decoder rather than this one -- and saying 0
    // here is how it finds out instead of discovering a copy.
    if (api != MP_GRAPHICS_NONE && api != MP_GRAPHICS_D3D11) {
        return MP_OK;
    }
    (void)config;
    (void)config_bytes;
    // Not 100: this is the operating system's decoder, and a module that
    // decodes the same stream itself should be preferred where one exists, the
    // same way `demux_mf` scores below every reader that splits properly.
    *out_score = 80;
    return MP_OK;
}

MpResult MP_CALL codec_open(MpCodec codec, const MpGraphicsDevice* device,
                            const std::uint8_t* config, std::uint32_t config_bytes,
                            MpVideoCodec** out) noexcept
try {
    if (out == nullptr || subtype_of(codec) == GUID_NULL) {
        return MP_ERR_INVALID;
    }
    if (!runtime().ok()) {
        log_line(MP_LOG_ERROR, "codec_mft: Media Foundation would not start");
        return MP_ERR_UNSUPPORTED;
    }
    if (device != nullptr && device->api != MP_GRAPHICS_D3D11) {
        // The host asked the wrong module. `probe` said so; this is the second
        // line of defence rather than the first.
        return MP_ERR_UNSUPPORTED;
    }

    auto c = std::unique_ptr<MpVideoCodec>(new (std::nothrow) MpVideoCodec());
    if (c == nullptr) {
        return MP_ERR_NO_MEMORY;
    }
    c->codec = codec;
    c->info.size = sizeof(MpVideoInfo);

    if (codec == MP_CODEC_H264) {
        c->avcc = mp::mft::parse_avcc(config, config_bytes);
        if (!c->avcc.valid) {
            log_line(MP_LOG_DEBUG, "codec_mft: no usable avcC in the stream configuration");
            return MP_ERR_FORMAT;
        }
    }

    const bool want_hardware =
        device != nullptr && device->device != nullptr;
    std::string why;
    if (!activate(c.get(), want_hardware, why)) {
        // **A machine with no hardware decoder still decodes.** Microsoft's
        // software H.264 transform is always there, it is deterministic, and it
        // is what makes a decoded frame something a test can hash.
        if (!want_hardware || !activate(c.get(), false, why)) {
            log_line(MP_LOG_DEBUG, ("codec_mft: " + why).c_str());
            return MP_ERR_UNSUPPORTED;
        }
    }

    if (want_hardware) {
        auto* d3d = static_cast<ID3D11Device*>(device->device);
        UINT token = 0;
        if (SUCCEEDED(::MFCreateDXGIDeviceManager(&token, c->manager.put())) &&
            SUCCEEDED(c->manager->ResetDevice(d3d, token)) &&
            SUCCEEDED(c->transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(c->manager.get())))) {
            d3d->AddRef();
            *c->device.put() = d3d;
            c->on_gpu = true;

            // **Ask for the binding rather than hoping for it.** A decoder's
            // output is allocated by the transform, and by default it is a
            // texture with D3D11_BIND_DECODER and nothing else -- which a
            // presenter cannot make a view over, so the frame would have to be
            // copied and the point of decoding on the GPU would be gone.
            // `MF_SA_D3D11_BINDFLAGS` is the documented way to say what the
            // allocation needs, and every hardware decoder that matters
            // honours it. A driver that does not gives back a texture without
            // the flag, and the presenter says which of the two happened.
            Com<IMFAttributes> outputs;
            if (SUCCEEDED(c->transform->GetOutputStreamAttributes(0, outputs.put()))) {
                outputs->SetUINT32(MF_SA_D3D11_BINDFLAGS,
                                   D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE);
            }
        } else {
            // The transform found by the hardware enumeration but unwilling to
            // take the device is a driver saying no. Its frames come back in
            // system memory, which is slower and correct.
            c->manager.reset();
            log_line(MP_LOG_INFO,
                     "codec_mft: the decoder would not take the D3D11 device; "
                     "frames will come back in system memory");
        }
    }

    if (!set_input_type(c.get(), why) || !set_output_type(c.get(), why)) {
        log_line(MP_LOG_DEBUG, ("codec_mft: " + why).c_str());
        return MP_ERR_FORMAT;
    }
    (void)read_output_format(c.get());

    c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    c->streaming = true;

    *out = c.release();
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL codec_close(MpVideoCodec* c) noexcept
{
    if (c == nullptr) {
        return;
    }
    release_frame(c);
    if (c->transform && c->streaming) {
        c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    delete c;
}

MpResult MP_CALL codec_get_format(MpVideoCodec* c, MpVideoInfo* out) noexcept
{
    if (c == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    if (!c->have_format && !read_output_format(c)) {
        return MP_ERR_UNSUPPORTED;
    }
    const std::uint32_t size = out->size;
    MpVideoInfo info = c->info;
    info.size = size;
    std::memcpy(out, &info, std::min<std::size_t>(size, sizeof(info)));
    return MP_OK;
}

MpResult MP_CALL codec_decode(MpVideoCodec* c, const void* packet, std::size_t bytes,
                              std::uint64_t pts) noexcept
try {
    if (c == nullptr || packet == nullptr || bytes == 0) {
        return MP_ERR_INVALID;
    }

    const std::uint8_t* payload = static_cast<const std::uint8_t*>(packet);
    std::size_t payload_bytes = bytes;
    if (c->codec == MP_CODEC_H264) {
        if (!mp::mft::to_annex_b(c->avcc, payload, bytes, c->needs_parameter_sets,
                                 c->annex_b)) {
            c->trouble = "a sample whose NAL lengths do not fit inside it";
            return MP_ERR_FORMAT;
        }
        payload = c->annex_b.data();
        payload_bytes = c->annex_b.size();
        c->needs_parameter_sets = false;
    }

    Com<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(payload_bytes), buffer.put()))) {
        return MP_ERR_NO_MEMORY;
    }
    BYTE* dst = nullptr;
    DWORD capacity = 0;
    if (FAILED(buffer->Lock(&dst, &capacity, nullptr))) {
        return MP_ERR_INTERNAL;
    }
    std::memcpy(dst, payload, payload_bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(payload_bytes));

    Com<IMFSample> sample;
    if (FAILED(::MFCreateSample(sample.put()))) {
        return MP_ERR_NO_MEMORY;
    }
    sample->AddBuffer(buffer.get());
    // MF counts in hundred-nanosecond units, which is what `get_format`
    // reports as the timescale so a host never has to know that here.
    sample->SetSampleTime(static_cast<LONGLONG>(pts));

    const HRESULT hr = c->transform->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The decoder is full: the host has frames to collect first. Not an
        // error, and saying so is what keeps a caller from dropping a packet.
        return MP_ERR_BUSY;
    }
    return SUCCEEDED(hr) ? MP_OK : MP_ERR_FORMAT;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_next_frame(MpVideoCodec* c, MpVideoFrame* out) noexcept
try {
    if (c == nullptr || out == nullptr) {
        return MP_ERR_INVALID;
    }
    release_frame(c);

    // A transform that allocates its own samples says so; one that does not
    // wants a buffer, which for a decoder producing NV12 means asking it how
    // big. `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES` is set by every decoder this
    // module uses, hardware or software.
    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (FAILED(c->transform->GetOutputStreamInfo(0, &stream_info))) {
        return MP_ERR_INTERNAL;
    }
    const bool provides =
        (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    MFT_OUTPUT_DATA_BUFFER output{};
    Com<IMFSample> ours;
    if (!provides) {
        Com<IMFMediaBuffer> buffer;
        if (FAILED(::MFCreateMemoryBuffer(stream_info.cbSize, buffer.put())) ||
            FAILED(::MFCreateSample(ours.put()))) {
            return MP_ERR_NO_MEMORY;
        }
        ours->AddBuffer(buffer.get());
        output.pSample = ours.get();
    }

    DWORD status = 0;
    const HRESULT hr = c->transform->ProcessOutput(0, 1, &output, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return MP_END; // nothing held back: feed it another packet
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // The decoder learned the real geometry from the bitstream, which is
        // the normal way it starts. Re-negotiating the output type is the
        // documented response and the next call produces a frame.
        std::string why;
        if (output.pEvents != nullptr) {
            output.pEvents->Release();
        }
        if (!set_output_type(c, why)) {
            c->trouble = why;
            return MP_ERR_FORMAT;
        }
        (void)read_output_format(c);
        return MP_ERR_BUSY; // ask again
    }
    if (output.pEvents != nullptr) {
        output.pEvents->Release();
    }
    if (FAILED(hr) || output.pSample == nullptr) {
        return FAILED(hr) ? MP_ERR_FORMAT : MP_END;
    }

    // Owned from here: `release_frame` lets it go on the next call.
    *c->out_sample.put() = output.pSample;
    if (!c->have_format && !read_output_format(c)) {
        return MP_ERR_INTERNAL;
    }

    LONGLONG time = 0;
    (void)c->out_sample->GetSampleTime(&time);

    const std::uint32_t size = out->size;
    *out = MpVideoFrame{};
    out->size = size;
    out->width = c->info.width;
    out->height = c->info.height;
    out->pts = static_cast<std::uint64_t>(time);

    if (FAILED(c->out_sample->ConvertToContiguousBuffer(c->out_buffer.put()))) {
        return MP_ERR_INTERNAL;
    }

    // **A texture if there is one, and nothing crosses the bus.** A hardware
    // decoder hands out a slice of an array it owns, which is why the frame
    // carries an index as well as a pointer.
    Com<IMFDXGIBuffer> dxgi;
    if (c->on_gpu &&
        SUCCEEDED(c->out_buffer->QueryInterface(IID_PPV_ARGS(dxgi.put())))) {
        if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(c->out_texture.put())))) {
            UINT slice = 0;
            (void)dxgi->GetSubresourceIndex(&slice);
            D3D11_TEXTURE2D_DESC desc{};
            c->out_texture->GetDesc(&desc);
            out->format = desc.Format == DXGI_FORMAT_P010 ? MP_PIXEL_P010 : MP_PIXEL_NV12;
            out->texture = c->out_texture.get();
            out->texture_index = slice;
            return MP_OK;
        }
    }

    // **Otherwise the planes, and there are three ways to reach them.** The
    // pitch is the thing worth having: a buffer locked flat has rows that are
    // contiguous only by luck, and NV12 from a decoder is usually padded to a
    // width the hardware liked. `IMF2DBuffer2` states it, `IMF2DBuffer` states
    // it, and a plain buffer does not -- and the software H.264 transform,
    // which is the one a machine with no GPU uses and therefore the one every
    // test runs, implements the middle of the three.
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;

    if (SUCCEEDED(c->out_buffer->QueryInterface(IID_PPV_ARGS(c->out_2d.put())))) {
        if (FAILED(c->out_2d->Lock2D(&scanline0, &pitch))) {
            return MP_ERR_INTERNAL;
        }
        c->out_lock = MpVideoCodec::Lock::two_d;
    } else {
        BYTE* start = nullptr;
        DWORD max_length = 0;
        DWORD length = 0;
        if (FAILED(c->out_buffer->Lock(&start, &max_length, &length))) {
            return MP_ERR_INTERNAL;
        }
        c->out_lock = MpVideoCodec::Lock::flat;
        scanline0 = start;
        pitch = c->default_stride;
    }

    // Bottom-up is what a negative pitch means, and `MpVideoFrame::stride` is
    // unsigned because no decoder in this tree produces one. Refusing is better
    // than reporting the magnitude and handing over a picture upside down.
    if (pitch <= 0) {
        c->trouble = "the decoder produced a bottom-up frame, which nothing here expects";
        return MP_ERR_UNSUPPORTED;
    }
    c->out_scanline0 = scanline0;
    c->out_pitch = pitch;

    // NV12 and P010 are both a full-size luma plane followed by a half-height
    // interleaved chroma plane at the same stride.
    out->format = MP_PIXEL_NV12;
    out->plane[0] = scanline0;
    out->stride[0] = static_cast<std::uint32_t>(pitch < 0 ? -pitch : pitch);
    out->plane[1] = scanline0 + static_cast<std::ptrdiff_t>(pitch) * c->info.height;
    out->stride[1] = out->stride[0];
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL codec_flush(MpVideoCodec* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    // **Drain, not flush.** `MFT_MESSAGE_COMMAND_DRAIN` asks for what is held
    // back; `MFT_MESSAGE_COMMAND_FLUSH` throws it away, which is what `reset`
    // wants and what a caller at the end of a file does not.
    c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    return SUCCEEDED(c->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))
               ? MP_OK
               : MP_ERR_INTERNAL;
}

MpResult MP_CALL codec_reset(MpVideoCodec* c) noexcept
{
    if (c == nullptr) {
        return MP_ERR_INVALID;
    }
    release_frame(c);
    c->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    // **The parameter sets go back in front.** A flushed decoder has forgotten
    // them, and the next packet after a seek is a keyframe whose SPS and PPS
    // live in the container rather than in the stream.
    c->needs_parameter_sets = true;
    c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return MP_OK;
}

const MpVideoCodecVtbl g_vtbl = {
    /* size       */ sizeof(MpVideoCodecVtbl),
    /* reserved   */ 0,
    /* probe      */ &codec_probe,
    /* open       */ &codec_open,
    /* close      */ &codec_close,
    /* get_format */ &codec_get_format,
    /* decode     */ &codec_decode,
    /* next_frame */ &codec_next_frame,
    /* flush      */ &codec_flush,
    /* reset      */ &codec_reset,
};

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_VCODEC,
    /* priority    */ 80,
    /* id          */ "codec_mft",
    /* name        */ "Media Foundation, as one transform and not as a pipeline",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi)
{
    if (host_abi != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

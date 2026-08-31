// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WASAPI sink: exclusive first, shared as a fallback the user has to choose.
//
// Everything interesting in this file is in negotiate(). The rest is plumbing.

#include "wave_format.hpp"
#include "win_headers.hpp"

#include <mediaperch/module.h>

#include <cstring>
#include <string>

namespace {

using mp::wasapi::ComPtr;

const MpHost* g_host = nullptr;

void log(MpLogLevel level, const char* message) noexcept
{
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(g_host->ctx, level, message);
    }
}

MpResult map_hr(HRESULT hr) noexcept
{
    switch (hr) {
    case S_OK:
        return MP_OK;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
    case AUDCLNT_E_BUFFER_SIZE_ERROR:
    case AUDCLNT_E_INVALID_DEVICE_PERIOD:
        return MP_ERR_FORMAT;
    case AUDCLNT_E_DEVICE_IN_USE:
        return MP_ERR_BUSY;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return MP_ERR_DENIED;
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return MP_ERR_DEVICE_LOST;
    case E_OUTOFMEMORY:
        return MP_ERR_NO_MEMORY;
    case E_INVALIDARG:
        return MP_ERR_INVALID;
    default:
        return MP_ERR_INTERNAL;
    }
}

std::string to_utf8(const wchar_t* wide)
{
    if (wide == nullptr) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == '\0') {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

void copy_into(char (&dst)[256], const std::string& src) noexcept
{
    const std::size_t n = src.size() < 255 ? src.size() : 255;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

HRESULT make_enumerator(ComPtr<IMMDeviceEnumerator>& out) noexcept
{
    return ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&out));
}

} // namespace

// The opaque handle from module.h, given a body here. Nothing outside this file
// ever sees the inside of it.
struct MpSink {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    ComPtr<IAudioClock2> clock2;
    HANDLE event = nullptr;
    MpShareMode mode = MP_SHARE_EXCLUSIVE;
    UINT32 buffer_frames = 0;
    UINT32 frame_bytes = 0;
    UINT32 sample_rate = 0;
    bool started = false;
    bool ready = false;

    ~MpSink()
    {
        if (started && client) {
            client->Stop();
        }
        if (event != nullptr) {
            ::CloseHandle(event);
        }
    }
};

namespace {

// --------------------------------------------------------------------------
// Enumeration
// --------------------------------------------------------------------------

MpResult MP_CALL sink_enumerate(std::uint32_t index, MpDeviceInfo* out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    std::memset(out, 0, sizeof(*out));
    out->size = sizeof(MpDeviceInfo);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = make_enumerator(enumerator);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    if (index >= count) {
        return MP_END;
    }

    ComPtr<IMMDevice> device;
    hr = collection->Item(index, &device);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    LPWSTR id = nullptr;
    hr = device->GetId(&id);
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    const std::string id_utf8 = to_utf8(id);

    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR default_id = nullptr;
        if (SUCCEEDED(default_device->GetId(&default_id))) {
            if (std::wcscmp(default_id, id) == 0) {
                out->flags |= MP_DEVICE_IS_DEFAULT;
            }
            ::CoTaskMemFree(default_id);
        }
    }
    ::CoTaskMemFree(id);
    copy_into(out->id, id_utf8);

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT name;
        ::PropVariantInit(&name);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            copy_into(out->name, to_utf8(name.pwszVal));
        }
        ::PropVariantClear(&name);
    }

    return MP_OK;
} catch (...) {
    return MP_ERR_INTERNAL;
}

// --------------------------------------------------------------------------
// Open and close
// --------------------------------------------------------------------------

MpResult MP_CALL sink_open(const char* device_id, MpShareMode mode, MpSink** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = make_enumerator(enumerator);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    ComPtr<IMMDevice> device;
    const std::wstring wide_id = to_wide(device_id);
    hr = wide_id.empty()
             ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
             : enumerator->GetDevice(wide_id.c_str(), &device);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    auto sink = new MpSink{};
    sink->device = device;
    sink->mode = mode;
    sink->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (sink->event == nullptr) {
        delete sink;
        return MP_ERR_INTERNAL;
    }

    *out = sink;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL sink_close(MpSink* sink) noexcept
{
    delete sink;
}

// --------------------------------------------------------------------------
// Negotiation -- the part this whole program is arranged around
// --------------------------------------------------------------------------

MpResult MP_CALL sink_negotiate(MpSink* sink, const MpFormat* want, MpFormat* accepted) noexcept
try {
    if (sink == nullptr || want == nullptr || accepted == nullptr) {
        return MP_ERR_INVALID;
    }

    WAVEFORMATEXTENSIBLE wfx{};
    if (!mp::wasapi::to_wave_format(*want, wfx)) {
        return MP_ERR_UNSUPPORTED;
    }
    auto* base = reinterpret_cast<WAVEFORMATEX*>(&wfx);

    // A fresh client for every attempt. An IAudioClient whose Initialize failed
    // is spent -- calling Initialize on it again returns AUDCLNT_E_ALREADY_
    // INITIALIZED or worse, and reusing it is the single most common way the
    // realign path below is got wrong.
    sink->ready = false;
    sink->render.Reset();
    sink->clock.Reset();
    sink->clock2.Reset();
    sink->client.Reset();

    HRESULT hr = sink->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(sink->client.GetAddressOf()));
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    const AUDCLNT_SHAREMODE share =
        sink->mode == MP_SHARE_EXCLUSIVE ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

    // Ask before initialising, but do not believe the answer: this is a filter
    // that saves a device round trip, not the decision. Drivers say yes here and
    // refuse in Initialize.
    WAVEFORMATEX* closest = nullptr;
    hr = sink->client->IsFormatSupported(share, base, &closest);
    const bool exact_hint = hr == S_OK;
    if (closest != nullptr) {
        // Shared mode answers S_FALSE with what it would convert to. Report it
        // rather than take it: the host classifies the result and will refuse a
        // conversion it did not ask for.
        if (share == AUDCLNT_SHAREMODE_SHARED) {
            MpFormat suggestion{};
            if (mp::wasapi::from_wave_format(*closest, suggestion)) {
                *accepted = suggestion;
            }
        }
        ::CoTaskMemFree(closest);
        if (share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
            return MP_ERR_FORMAT;
        }
        return MP_ERR_FORMAT;
    }
    if (!exact_hint && share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
        return MP_ERR_FORMAT;
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    hr = sink->client->GetDevicePeriod(&default_period, &minimum_period);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    if (share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
        hr = sink->client->Initialize(share, flags, minimum_period, minimum_period, base,
                                      nullptr);

        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // Documented, normal, and not an error. The device has told us the
            // period it will actually accept by way of the buffer size it would
            // have allocated; ask for exactly that instead.
            UINT32 aligned = 0;
            const HRESULT size_hr = sink->client->GetBufferSize(&aligned);
            if (FAILED(size_hr) || aligned == 0) {
                return map_hr(size_hr);
            }
            const auto realigned = static_cast<REFERENCE_TIME>(
                10'000'000.0 / base->nSamplesPerSec * aligned + 0.5);

            log(MP_LOG_INFO, "buffer size not aligned; retrying on a fresh client");

            sink->client.Reset();
            hr = sink->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(sink->client.GetAddressOf()));
            if (FAILED(hr)) {
                return map_hr(hr);
            }
            hr = sink->client->Initialize(share, flags, realigned, realigned, base, nullptr);
        }
    } else {
        // Shared mode lets the engine pick the buffer; 0 means "your choice",
        // which is the only value that works with an event-driven shared stream.
        hr = sink->client->Initialize(share, flags, 0, 0, base, nullptr);
    }

    if (FAILED(hr)) {
        return map_hr(hr);
    }

    hr = sink->client->SetEventHandle(sink->event);
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    hr = sink->client->GetBufferSize(&sink->buffer_frames);
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    hr = sink->client->GetService(IID_PPV_ARGS(&sink->render));
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    // Position is best-effort: a device that cannot report it still plays.
    sink->client->GetService(IID_PPV_ARGS(&sink->clock));
    if (sink->clock) {
        sink->clock.As(&sink->clock2);
    }

    sink->frame_bytes = mp::wasapi::frame_bytes_of(*want);
    sink->sample_rate = want->sample_rate;
    sink->ready = true;

    *accepted = *want;
    return MP_OK;
} catch (...) {
    return MP_ERR_INTERNAL;
}

MpResult MP_CALL sink_get_period(MpSink* sink, std::uint32_t* frames) noexcept
{
    if (sink == nullptr || frames == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    *frames = sink->buffer_frames;
    return MP_OK;
}

// --------------------------------------------------------------------------
// Transport
// --------------------------------------------------------------------------

MpResult MP_CALL sink_start(MpSink* sink) noexcept
{
    if (sink == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    const HRESULT hr = sink->client->Start();
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    sink->started = true;
    return MP_OK;
}

MpResult MP_CALL sink_stop(MpSink* sink) noexcept
{
    if (sink == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    if (!sink->started) {
        return MP_OK;
    }
    const HRESULT hr = sink->client->Stop();
    sink->started = false;
    return FAILED(hr) ? map_hr(hr) : MP_OK;
}

// MP_RT from here down: no allocation, no logging, no locking.

MpResult MP_CALL sink_wait(MpSink* sink, std::uint32_t timeout_ms) noexcept
{
    const DWORD waited = ::WaitForSingleObject(sink->event, timeout_ms);
    if (waited == WAIT_OBJECT_0) {
        return MP_OK;
    }
    if (waited == WAIT_TIMEOUT) {
        return MP_TIMEOUT;
    }
    return MP_ERR_INTERNAL;
}

MpResult MP_CALL sink_acquire(MpSink* sink, void** ptr, std::uint32_t* frames) noexcept
{
    UINT32 wanted = sink->buffer_frames;

    if (sink->mode == MP_SHARE_SHARED) {
        // Shared mode hands back only what has drained; exclusive always gives
        // the whole buffer, which is what makes its render loop a straight line.
        UINT32 padding = 0;
        const HRESULT hr = sink->client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            return map_hr(hr);
        }
        wanted = sink->buffer_frames - padding;
        if (wanted == 0) {
            *ptr = nullptr;
            *frames = 0;
            return MP_OK;
        }
    }

    BYTE* buffer = nullptr;
    const HRESULT hr = sink->render->GetBuffer(wanted, &buffer);
    if (FAILED(hr)) {
        return map_hr(hr);
    }
    *ptr = buffer;
    *frames = wanted;
    return MP_OK;
}

MpResult MP_CALL sink_commit(MpSink* sink, std::uint32_t frames, std::uint32_t flags) noexcept
{
    if (frames == 0) {
        return MP_OK;
    }
    const DWORD native = (flags & MP_COMMIT_SILENT) != 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
    const HRESULT hr = sink->render->ReleaseBuffer(frames, native);
    return FAILED(hr) ? map_hr(hr) : MP_OK;
}

MpResult MP_CALL sink_get_position(MpSink* sink, std::uint64_t* frames,
                                   std::uint64_t* qpc) noexcept
{
    if (sink == nullptr || frames == nullptr || qpc == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }

    if (sink->clock2) {
        UINT64 device_frames = 0;
        UINT64 device_qpc = 0;
        const HRESULT hr = sink->clock2->GetDevicePosition(&device_frames, &device_qpc);
        if (SUCCEEDED(hr)) {
            *frames = device_frames;
            *qpc = device_qpc;
            return MP_OK;
        }
    }

    if (sink->clock) {
        UINT64 frequency = 0;
        UINT64 position = 0;
        UINT64 position_qpc = 0;
        if (SUCCEEDED(sink->clock->GetFrequency(&frequency)) && frequency != 0 &&
            SUCCEEDED(sink->clock->GetPosition(&position, &position_qpc))) {
            *frames = position * sink->sample_rate / frequency;
            *qpc = position_qpc;
            return MP_OK;
        }
    }

    return MP_ERR_UNSUPPORTED;
}

// --------------------------------------------------------------------------
// The module
// --------------------------------------------------------------------------

MpResult MP_CALL module_init(const MpHost* host) noexcept
{
    g_host = host;
    return MP_OK;
}

void MP_CALL module_shutdown() noexcept
{
    g_host = nullptr;
}

const MpSinkVtbl g_sink_vtbl = {
    /* size          */ sizeof(MpSinkVtbl),
    /* reserved      */ 0,
    /* enumerate     */ &sink_enumerate,
    /* open          */ &sink_open,
    /* negotiate     */ &sink_negotiate,
    /* get_period    */ &sink_get_period,
    /* start         */ &sink_start,
    /* stop          */ &sink_stop,
    /* close         */ &sink_close,
    /* wait          */ &sink_wait,
    /* acquire       */ &sink_acquire,
    /* commit        */ &sink_commit,
    /* get_position  */ &sink_get_position,
};

const MpModuleDesc g_desc = {
    /* size        */ sizeof(MpModuleDesc),
    /* abi_version */ MP_ABI_VERSION,
    /* flags       */ 0,
    /* version     */ MP_MAKE_VERSION(0, 1, 0),
    /* kind        */ MP_KIND_SINK,
    /* priority    */ 100,
    /* id          */ "sink_wasapi",
    /* name        */ "WASAPI (exclusive and shared)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_sink_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi_version)
{
    // An ABI mismatch has exactly one correct answer, and it is not "try anyway".
    if (host_abi_version != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

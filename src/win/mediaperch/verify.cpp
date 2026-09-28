// SPDX-License-Identifier: GPL-3.0-or-later
#include "mediaperch/verify.hpp"

#include "mediaperch/platform.hpp"
#include "mediaperch/win_headers.hpp"

#include "com_out.hpp"
#include "wave_format.hpp"

#include <audioclient.h>
#include <bcrypt.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace mp::win {
namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

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

std::wstring to_wide(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), needed);
    return out;
}

MpResult map_hr(HRESULT hr) noexcept
{
    switch (hr) {
    case S_OK: return MP_OK;
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return MP_ERR_FORMAT;
    case AUDCLNT_E_DEVICE_IN_USE: return MP_ERR_BUSY;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return MP_ERR_DENIED;
    case AUDCLNT_E_DEVICE_INVALIDATED: return MP_ERR_DEVICE_LOST;
    default: return MP_ERR_INTERNAL;
    }
}

} // namespace

// --------------------------------------------------------------------------
// SHA-256
// --------------------------------------------------------------------------

Sha256::Sha256()
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                      nullptr, 0))) {
        return;
    }
    algorithm_ = algorithm;

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        algorithm_ = nullptr;
        return;
    }
    hash_ = hash;
}

Sha256::~Sha256()
{
    if (hash_ != nullptr) {
        ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    }
    if (algorithm_ != nullptr) {
        ::BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    }
}

void Sha256::update(const void* data, std::size_t bytes) noexcept
{
    if (hash_ == nullptr || data == nullptr) {
        return;
    }
    // BCryptHashData takes its input as bytes it could write, though it only
    // reads them, so it is handed a copy of its own a chunk at a time rather
    // than the caller's bytes with their const taken off. Copying a chunk costs
    // a fraction of hashing it, and a chunk also keeps each call's length
    // within the ULONG it is.
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::array<UCHAR, 16384> chunk{};
    while (bytes > 0) {
        const std::size_t now = std::min(bytes, chunk.size());
        std::copy_n(p, now, chunk.data());
        // Checked, because a digest of most of the bytes is worse than none:
        // `verify` exists to say whether what reached the device was what was
        // sent, and a hash that silently skipped a chunk would say yes.
        if (!BCRYPT_SUCCESS(::BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                                             chunk.data(), static_cast<ULONG>(now), 0))) {
            failed_ = true;
            return;
        }
        p += now;
        bytes -= now;
    }
}

std::string Sha256::hex()
{
    if (hash_ == nullptr || failed_) {
        return {};
    }
    std::array<UCHAR, 32> digest{};
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                                           digest.data(),
                                           static_cast<ULONG>(digest.size()), 0))) {
        return {};
    }

    static constexpr char nibbles[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        out.push_back(nibbles[byte >> 4]);
        out.push_back(nibbles[byte & 0x0F]);
    }
    return out;
}

std::string Sha256::of(const void* data, std::size_t bytes)
{
    Sha256 hash;
    hash.update(data, bytes);
    return hash.hex();
}

// --------------------------------------------------------------------------
// Capture
// --------------------------------------------------------------------------

Capture::~Capture()
{
    stop();
    release();
}

void Capture::release() noexcept
{
    if (capture_ != nullptr) {
        static_cast<IAudioCaptureClient*>(capture_)->Release();
        capture_ = nullptr;
    }
    if (client_ != nullptr) {
        static_cast<IAudioClient*>(client_)->Release();
        client_ = nullptr;
    }
    if (device_ != nullptr) {
        static_cast<IMMDevice*>(device_)->Release();
        device_ = nullptr;
    }
    if (event_ != nullptr) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
}

MpResult Capture::enumerate(std::uint32_t index, MpDeviceInfo& out) noexcept
try {
    std::memset(&out, 0, sizeof(out));
    out.size = sizeof(MpDeviceInfo);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = mp::wasapi::make_enumerator(enumerator);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return MP_ERR_INTERNAL;
    }
    if (index >= count) {
        return MP_END;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(index, &device))) {
        return MP_ERR_INTERNAL;
    }

    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id))) {
        const std::string utf8 = to_utf8(id);
        const std::size_t n = std::min<std::size_t>(utf8.size(), 255);
        std::memcpy(out.id, utf8.data(), n);
        ::CoTaskMemFree(id);
    }

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT name;
        ::PropVariantInit(&name);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            const std::string utf8 = to_utf8(name.pwszVal);
            const std::size_t n = std::min<std::size_t>(utf8.size(), 255);
            std::memcpy(out.name, utf8.data(), n);
        }
        ::PropVariantClear(&name);
    }
    return MP_OK;
} catch (...) {
    return MP_ERR_INTERNAL;
}

std::string Capture::find_by_name(const std::string& needle)
{
    MpDeviceInfo info{};
    for (std::uint32_t index = 0;; ++index) {
        if (enumerate(index, info) != MP_OK) {
            return {};
        }
        if (std::string_view{info.name}.find(needle) != std::string_view::npos) {
            return info.id;
        }
    }
}

MpResult Capture::open(const std::string& device_id)
{
    release();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = mp::wasapi::make_enumerator(enumerator);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    ComPtr<IMMDevice> device;
    const std::wstring wide = to_wide(device_id);
    hr = wide.empty() ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)
                      : enumerator->GetDevice(wide.c_str(), &device);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    device_ = device.Detach();
    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event_ != nullptr ? MP_OK : MP_ERR_INTERNAL;
}

MpResult Capture::negotiate(const MpFormat& want)
{
    if (device_ == nullptr) {
        return MP_ERR_INVALID;
    }

    WAVEFORMATEXTENSIBLE wfx{};
    if (!mp::wasapi::to_wave_format(want, wfx)) {
        return MP_ERR_UNSUPPORTED;
    }
    WAVEFORMATEX* base = &wfx.Format;

    auto activate = [this](IAudioClient** out) {
        return mp::wasapi::activate(static_cast<IMMDevice*>(device_), *out);
    };

    if (capture_ != nullptr) {
        static_cast<IAudioCaptureClient*>(capture_)->Release();
        capture_ = nullptr;
    }
    if (client_ != nullptr) {
        static_cast<IAudioClient*>(client_)->Release();
        client_ = nullptr;
    }

    IAudioClient* client = nullptr;
    HRESULT hr = activate(&client);
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    hr = client->GetDevicePeriod(&default_period, &minimum_period);
    if (FAILED(hr)) {
        client->Release();
        return map_hr(hr);
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            minimum_period, minimum_period, base, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        if (SUCCEEDED(client->GetBufferSize(&aligned)) && aligned != 0) {
            const auto realigned = static_cast<REFERENCE_TIME>(
                10'000'000.0 / base->nSamplesPerSec * aligned + 0.5);
            client->Release();
            client = nullptr;
            hr = activate(&client);
            if (SUCCEEDED(hr)) {
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, realigned,
                                        realigned, base, nullptr);
            }
        }
    }
    if (FAILED(hr)) {
        if (client != nullptr) {
            client->Release();
        }
        return map_hr(hr);
    }

    hr = client->SetEventHandle(event_);
    if (SUCCEEDED(hr)) {
        hr = client->GetBufferSize(&buffer_frames_);
    }
    IAudioCaptureClient* capture = nullptr;
    if (SUCCEEDED(hr)) {
        hr = mp::wasapi::service(client, capture);
    }
    if (FAILED(hr)) {
        client->Release();
        return map_hr(hr);
    }

    client_ = client;
    capture_ = capture;
    frame_bytes_ = mp::wasapi::frame_bytes_of(want);
    return MP_OK;
}

MpResult Capture::start(std::size_t reserve_bytes)
{
    if (client_ == nullptr || capture_ == nullptr) {
        return MP_ERR_INVALID;
    }
    data_.clear();
    data_.reserve(reserve_bytes);
    discontinuities_ = 0;
    silent_ = 0;

    const HRESULT hr = static_cast<IAudioClient*>(client_)->Start();
    if (FAILED(hr)) {
        return map_hr(hr);
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { record_loop(); });
    return MP_OK;
}

void Capture::stop() noexcept
{
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (client_ != nullptr) {
        static_cast<IAudioClient*>(client_)->Stop();
    }
}

void Capture::record_loop() noexcept
{
    RenderThreadHooks hooks;
    hooks.enter();

    auto* capture = static_cast<IAudioCaptureClient*>(capture_);

    while (running_.load(std::memory_order_acquire)) {
        if (::WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) {
            break;
        }

        BYTE* buffer = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT hr = capture->GetBuffer(&buffer, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            break;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            ++discontinuities_;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
            ++silent_;
        }

        if (frames != 0 && buffer != nullptr) {
            const std::size_t bytes = static_cast<std::size_t>(frames) * frame_bytes_;
            // Appending allocates, which is exactly what a render thread may not
            // do -- but this is a recorder, not a renderer, and dropping samples
            // to keep the loop clean would defeat its whole purpose. The reserve
            // in `start` is sized so that it does not happen in practice.
            data_.insert(data_.end(), buffer, buffer + bytes);
        }
        capture->ReleaseBuffer(frames);
    }

    hooks.leave();
}

// --------------------------------------------------------------------------

float endpoint_volume(const std::string& device_id, bool capture) noexcept
try {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(mp::wasapi::make_enumerator(enumerator))) {
        return -1.0F;
    }
    ComPtr<IMMDevice> device;
    const std::wstring wide = to_wide(device_id);
    const HRESULT hr =
        wide.empty()
            ? enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole,
                                                  &device)
            : enumerator->GetDevice(wide.c_str(), &device);
    if (FAILED(hr)) {
        return -1.0F;
    }

    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(mp::wasapi::activate(device.Get(), volume))) {
        return -1.0F;
    }
    float level = -1.0F;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&level))) {
        return -1.0F;
    }
    return level;
} catch (...) {
    return -1.0F;
}

std::size_t find_bytes(const std::vector<std::uint8_t>& haystack, const std::uint8_t* needle,
                       std::size_t needle_bytes, std::size_t from) noexcept
{
    if (needle == nullptr || needle_bytes == 0 || haystack.size() < needle_bytes) {
        return std::string::npos;
    }
    const auto* begin = haystack.data();
    const auto* end = begin + haystack.size();
    const auto* found =
        std::search(begin + std::min(from, haystack.size()), end, needle, needle + needle_bytes);
    return found == end ? std::string::npos : static_cast<std::size_t>(found - begin);
}

} // namespace mp::win

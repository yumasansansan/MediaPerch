// SPDX-License-Identifier: GPL-3.0-or-later
//
// ASIO -- and the second implementation of `MpSinkVtbl`, which is most of why
// it is here.
//
// **It is not a lower layer than WASAPI exclusive and buys no accuracy.**
// docs/plan.md §5 settles that: an exclusive-mode stream has no mixer, no APO
// and no resampler in front of it, and for a WaveRT driver `GetBuffer` hands
// over the hardware buffer itself. There is nowhere lower for user-mode code to
// stand. What ASIO buys is one format Windows has no wire for -- **DSD without
// the DoP wrapper** -- and one thing this file did not set out to buy and is
// the more useful of the two: an answer to whether `MpSinkVtbl` is a sink's
// abstraction or WASAPI's shape written down. §15 says not to add an interface
// until its second implementation exists. This is that implementation.
//
// **The two differ in three ways, and each one is a real finding.**
//
// *One.* WASAPI is pull and ASIO is push. The ABI is
// `wait` / `acquire` / `commit` -- the host asks the device for a buffer -- and
// an ASIO driver instead *calls* the host on its own thread, twice a period,
// expecting the buffers it pre-allocated to be full. So this sink keeps one
// staging buffer, `wait` blocks on an event the driver's callback sets, and the
// callback copies out of the staging buffer. Everything the host does is
// unchanged; the adaptation is nine lines and an atomic.
//
// *Two.* WASAPI's buffer is interleaved and ASIO's is one buffer per channel.
// `acquire` is documented as handing out "the device buffer", which is a
// WASAPI-shaped sentence: there is no single ASIO buffer to hand out. It hands
// out the staging buffer instead, and the callback de-interleaves. **That costs
// a copy WASAPI does not need**, and it is not avoidable -- a host that wrote
// deinterleaved would just move the copy to the other side, and Path A's whole
// point is that it does not touch the samples at all.
//
// *Three.* WASAPI negotiates and ASIO declares. `IsFormatSupported` is a
// question; an ASIO driver's sample type is whatever `getChannelInfo` says and
// there is no asking for another. So `negotiate` here is a comparison rather
// than a proposal, which the ABI turns out to accommodate exactly: the host
// offers candidates in order and takes the first the sink accepts.
//
// **Native DSD, and why no new format was needed for it.** The graph carries
// DSD as DoP -- two DSD bytes in a 24-bit frame under an alternating marker --
// because that is what a PCM link can carry. An ASIO driver in DSD mode is not
// a PCM link, so this sink takes the framing back off: `MP_ENCODING_DOP` means
// *DSD, in the frames a PCM link carries*, and a sink whose link is not PCM
// unwraps it. That is not a workaround, it is where the decision belongs --
// **the codec's format is fixed before the sink is chosen**, so a codec cannot
// produce a sink-specific form, and the sink is the only party that knows which
// link it is. Nothing was lost on the way: the pack and the unpack are inverses
// and the test below proves it.
//
// The SDK is Steinberg's, used under the GPLv3 option it gained in October
// 2025. Only its headers are used: the C wrapper in `asio.cpp` keeps one global
// driver and the enumeration in `host/` is old code with `char[32]` in it, and
// this tree reads its own registry for the same reason it opens its own files.

#include "dop_unpack.hpp"

#include <mediaperch/module.h>

#include "module_log.hpp"

#include <windows.h>

#include <unknwn.h>

#include <asiosys.h>
// asio.h and iasiodrv.h in that order: the second includes the first and
// declares IASIO, and neither guards against being included twice.
#include <asio.h>
#include <iasiodrv.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

const MpHost* g_host = nullptr;

void log_fmt(MpLogLevel level, const char* format, ...) noexcept
{
    // The body is modules/shared/module_log: twelve copies of it had drifted to
    // three buffer sizes. Only the wrapper stays, because a `...` function
    // cannot forward to another one -- the va_list has to be made here.
    va_list args;
    va_start(args, format);
    mp::log::vfmt(g_host, level, format, args);
    va_end(args);
}

std::string to_utf8(const wchar_t* wide)
{
    if (wide == nullptr || *wide == L'\0') {
        return {};
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == '\0') {
        return {};
    }
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), len);
    return out;
}

void copy_into(char (&dst)[256], const std::string& text)
{
    const std::size_t n = text.size() < 255 ? text.size() : 255;
    std::memcpy(dst, text.data(), n);
    dst[n] = '\0';
}

/// One registered driver, as the registry describes it.
struct Registered {
    std::wstring key;   ///< the subkey name, which is what a person sees
    std::wstring clsid; ///< "{...}", and the id this module hands out
    std::wstring description;
    /// The driver DLL, from `InprocServer32`. Read here because the module
    /// loads it itself; `sink_open` says why.
    std::wstring dll;
};

/// The DLL a CLSID's `InprocServer32` names, or empty.
std::wstring inproc_server(const std::wstring& clsid)
{
    if (clsid.empty()) {
        return {};
    }
    const std::wstring path = L"CLSID\\" + clsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t data[1024];
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LSTATUS r = ::RegQueryValueExW(key, nullptr, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(data), &bytes);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    const std::size_t chars = bytes / sizeof(wchar_t);
    data[chars < 1024 ? chars : 1023] = L'\0';
    return std::wstring{data};
}

/// Every driver under HKLM\\SOFTWARE\\ASIO, in the order the registry gives.
///
/// Written here rather than taken from the SDK's `asiolist.cpp`, which is 1990s
/// code with fixed `char[32]` buffers in it. This tree opens its own files for
/// the same reason.
std::vector<Registered> registered_drivers()
{
    std::vector<Registered> out;
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return out;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD name_len = 256;
        if (::RegEnumKeyExW(root, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, name, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }
        const auto read = [&](const wchar_t* value) -> std::wstring {
            wchar_t data[512];
            DWORD bytes = sizeof(data);
            DWORD type = 0;
            if (::RegQueryValueExW(key, value, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(data), &bytes) != ERROR_SUCCESS ||
                type != REG_SZ) {
                return {};
            }
            data[(bytes / sizeof(wchar_t)) < 512 ? (bytes / sizeof(wchar_t)) : 511] = L'\0';
            return std::wstring{data};
        };
        Registered entry;
        entry.key = name;
        entry.clsid = read(L"CLSID");
        entry.description = read(L"Description");
        ::RegCloseKey(key);
        entry.dll = inproc_server(entry.clsid);
        // A key with no CLSID names nothing that can be instantiated.
        if (!entry.clsid.empty()) {
            out.push_back(std::move(entry));
        }
    }
    ::RegCloseKey(root);
    return out;
}

/// **ASIO's callbacks carry no user pointer**, so the open sink is a global.
///
/// That is a property of the interface rather than a shortcut: `ASIOCallbacks`
/// is four bare function pointers, and the driver has nowhere to put a `this`.
/// It is also not a limitation in practice -- an ASIO driver is held by one
/// process at a time and this module opens one at a time -- but it is why the
/// second `open` while a sink is live is refused rather than queued.
std::atomic<MpSink*> g_open{nullptr};

// The DoP layout and its inverse live in dop_unpack.hpp, which has no Windows
// in it so that tests/dop_test.cpp can hold them against what `codec_dsd`
// writes. Nothing here reimplements them.
using mp::asio::k_dop_frame_bytes;
using mp::asio::k_dsd_bytes_per_dop_frame;

} // namespace

struct MpSink {
    IASIO* driver = nullptr;
    /// The driver DLL, loaded by this module rather than by COM. Released
    /// after the interface, and never before.
    HMODULE dll = nullptr;
    std::wstring clsid;
    std::string name;

    long outputs = 0;
    long buffer_frames = 0; ///< the driver's, in its own samples
    ASIOSampleType type = ASIOSTLastEntry;
    std::vector<ASIOBufferInfo> buffers;

    MpFormat format{};
    /// Bytes one frame of `format` takes in the staging buffer.
    std::uint32_t frame_bytes = 0;
    /// Frames of `format` in one period, which is what `get_period` reports.
    std::uint32_t period_frames = 0;
    /// Bytes one output channel takes in one of the driver's buffers.
    std::uint32_t channel_bytes = 0;

    bool dsd = false;
    bool dsd_lsb_first = false;
    bool started = false;
    bool ready = false;

    /// One period, interleaved, as the host writes it.
    std::vector<std::uint8_t> staging;
    /// Frames the host committed, read by the driver's thread.
    std::atomic<std::uint32_t> filled{0};
    /// Set when the staging buffer is free again.
    HANDLE free_event = nullptr;
    /// Periods the driver asked for and did not get. The host's own underrun
    /// count cannot see these: they happen on the driver's thread.
    std::atomic<std::uint64_t> starved{0};
    std::atomic<std::uint64_t> played{0};

    ~MpSink()
    {
        if (driver != nullptr) {
            if (started) {
                driver->stop();
            }
            if (ready) {
                driver->disposeBuffers();
            }
            driver->Release();
        }
        if (free_event != nullptr) {
            ::CloseHandle(free_event);
        }
        // After the interface, and only after: the code the vtable points at
        // lives in this module.
        if (dll != nullptr) {
            ::FreeLibrary(dll);
        }
    }
};

namespace {

// --------------------------------------------------------------------------
// The driver's thread
// --------------------------------------------------------------------------

/// Interleaved host frames into the driver's per-channel buffers.
///
/// **MP_RT in everything but name.** It runs on the driver's own thread, which
/// is the one that must not be late, so there is no allocation, no lock and no
/// logging here -- the same rules the ABI puts on `acquire` and `commit`.
void fill_buffers(MpSink* s, long index) noexcept
{
    const std::uint32_t frames = s->filled.load(std::memory_order_acquire);
    const auto channels = static_cast<std::size_t>(s->outputs);

    if (frames == 0) {
        // Nothing was committed in time. Silence, and count it: a period the
        // driver played without us is exactly what an underrun is, and the
        // host cannot see it from its side of the handoff.
        for (std::size_t c = 0; c < channels; ++c) {
            std::memset(s->buffers[c].buffers[index], 0, s->channel_bytes);
        }
        s->starved.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uint8_t* in = s->staging.data();
    if (s->dsd) {
        // **DoP back to DSD**, which `dop_unpack.hpp` states and a test checks.
        for (std::size_t c = 0; c < channels; ++c) {
            auto* out = static_cast<std::uint8_t*>(s->buffers[c].buffers[index]);
            mp::asio::unpack_channel(in, channels, c, frames, s->dsd_lsb_first, out);
            const std::size_t wrote =
                static_cast<std::size_t>(frames) * k_dsd_bytes_per_dop_frame;
            if (wrote < s->channel_bytes) {
                std::memset(out + wrote, 0, s->channel_bytes - wrote);
            }
        }
    } else {
        const std::size_t width = s->frame_bytes / channels;
        for (std::size_t c = 0; c < channels; ++c) {
            auto* out = static_cast<std::uint8_t*>(s->buffers[c].buffers[index]);
            for (std::uint32_t f = 0; f < frames; ++f) {
                std::memcpy(out + f * width,
                            in + static_cast<std::size_t>(f) * s->frame_bytes + c * width,
                            width);
            }
            const std::size_t wrote = static_cast<std::size_t>(frames) * width;
            if (wrote < s->channel_bytes) {
                std::memset(out + wrote, 0, s->channel_bytes - wrote);
            }
        }
    }
    s->played.fetch_add(frames, std::memory_order_relaxed);
    s->filled.store(0, std::memory_order_release);
    ::SetEvent(s->free_event);
}

void buffer_switch(long index, ASIOBool) noexcept
{
    MpSink* s = g_open.load(std::memory_order_acquire);
    if (s == nullptr || index < 0 || index > 1) {
        return;
    }
    fill_buffers(s, index);
    // Drivers that ask for it want to be told the buffer is ready. Ones that do
    // not answer ASE_NotPresent to `outputReady` at init and it is never called.
    s->driver->outputReady();
}

ASIOTime* buffer_switch_time_info(ASIOTime* params, long index, ASIOBool direct) noexcept
{
    buffer_switch(index, direct);
    return params;
}

void sample_rate_did_change(ASIOSampleRate) noexcept
{
    // The driver telling us, not asking. The host finds out through
    // `get_position` drifting and rebuilds, which is the same path a device
    // that goes away takes.
}

long asio_message(long selector, long value, void*, double*) noexcept
{
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioResetRequest || value == kAsioEngineVersion ||
                value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                value == kAsioSupportsTimeInfo || value == kAsioOverload)
                   ? 1L
                   : 0L;
    case kAsioEngineVersion: return 2L;
    case kAsioSupportsTimeInfo: return 1L;
    // A reset request means the driver wants to be torn down and rebuilt. This
    // module cannot do that from the driver's own thread, so it says yes and
    // the host's rebuild path -- the one a lost device already takes -- is what
    // actually does it, on noticing the position stop.
    case kAsioResetRequest: return 1L;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged: return 1L;
    case kAsioOverload:
        // The driver is late, which is its own underrun rather than ours.
        if (MpSink* s = g_open.load(std::memory_order_acquire); s != nullptr) {
            s->starved.fetch_add(1, std::memory_order_relaxed);
        }
        return 1L;
    default: return 0L;
    }
}

ASIOCallbacks g_callbacks = {
    &buffer_switch,
    &sample_rate_did_change,
    &asio_message,
    &buffer_switch_time_info,
};

// --------------------------------------------------------------------------
// Formats
// --------------------------------------------------------------------------

/// Bytes one sample of an ASIO type occupies, or 0 for one this module will
/// not pretend about.
///
/// **The right-aligned types are refused on purpose.** `ASIOSTInt32LSB24` is 24
/// bits in the low end of a 32-bit word; `MP_SAMPLE_S24_IN_32` is 24 bits in
/// the *high* end, which is what `WAVEFORMATEXTENSIBLE` means and what this
/// tree's repack produces. Accepting one as the other would play about 48 dB
/// too quietly, which is exactly the mistake `repack.hpp` has a test for. A
/// shift would fix it and a shift is a conversion, which a bit-exact path may
/// not contain.
std::uint32_t asio_container_bytes(ASIOSampleType type) noexcept
{
    switch (type) {
    case ASIOSTInt16LSB: return 2;
    case ASIOSTInt24LSB: return 3;
    case ASIOSTInt32LSB:
    case ASIOSTFloat32LSB: return 4;
    default: return 0;
    }
}

bool asio_is_float(ASIOSampleType type) noexcept
{
    return type == ASIOSTFloat32LSB;
}

/// Bytes one sample of an `MpSampleType` occupies, or 0.
std::uint32_t host_container_bytes(MpSampleType type) noexcept
{
    switch (type) {
    case MP_SAMPLE_S16: return 2;
    case MP_SAMPLE_S24_PACKED: return 3;
    case MP_SAMPLE_S24_IN_32:
    case MP_SAMPLE_S32:
    case MP_SAMPLE_F32: return 4;
    default: return 0;
    }
}

bool is_dsd_type(ASIOSampleType type) noexcept
{
    return type == ASIOSTDSDInt8LSB1 || type == ASIOSTDSDInt8MSB1 || type == ASIOSTDSDInt8NER8;
}

const char* type_name(ASIOSampleType type) noexcept
{
    switch (type) {
    case ASIOSTInt16LSB: return "Int16LSB";
    case ASIOSTInt24LSB: return "Int24LSB";
    case ASIOSTInt32LSB: return "Int32LSB";
    case ASIOSTFloat32LSB: return "Float32LSB";
    case ASIOSTFloat64LSB: return "Float64LSB";
    case ASIOSTInt32LSB16: return "Int32LSB16";
    case ASIOSTInt32LSB18: return "Int32LSB18";
    case ASIOSTInt32LSB20: return "Int32LSB20";
    case ASIOSTInt32LSB24: return "Int32LSB24";
    case ASIOSTDSDInt8LSB1: return "DSDInt8LSB1";
    case ASIOSTDSDInt8MSB1: return "DSDInt8MSB1";
    case ASIOSTDSDInt8NER8: return "DSDInt8NER8";
    default: return "an ASIO type this module does not name";
    }
}

// --------------------------------------------------------------------------
// The vtable
// --------------------------------------------------------------------------

MpResult MP_CALL sink_enumerate(std::uint32_t index, MpDeviceInfo* out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    std::memset(out, 0, sizeof(*out));
    out->size = sizeof(MpDeviceInfo);

    const auto drivers = registered_drivers();
    if (index >= drivers.size()) {
        return MP_END;
    }
    const Registered& d = drivers[index];
    // **The CLSID is the id and the key is the name.** A driver's registry key
    // is what a person recognises and is free to be renamed; the CLSID is what
    // `CoCreateInstance` takes and does not move.
    copy_into(out->id, to_utf8(d.clsid.c_str()));
    copy_into(out->name,
              to_utf8((d.description.empty() ? d.key : d.description).c_str()));
    return MP_OK;
} catch (...) {
    return MP_ERR_INTERNAL;
}

MpResult MP_CALL sink_open(const char* device_id, MpShareMode mode, MpSink** out) noexcept
try {
    if (out == nullptr) {
        return MP_ERR_INVALID;
    }
    *out = nullptr;

    // **ASIO is exclusive and has no other mode**, so asking for shared is a
    // question this module has no honest answer to. Saying so is better than
    // taking the device and quietly giving exclusive behaviour to a caller who
    // asked not to have it.
    if (mode == MP_SHARE_SHARED) {
        log_fmt(MP_LOG_DEBUG, "ASIO has no shared mode; sink_wasapi is what that asks for");
        return MP_ERR_UNSUPPORTED;
    }
    if (g_open.load(std::memory_order_acquire) != nullptr) {
        // See `g_open`: the callbacks carry no user pointer, so there is one.
        return MP_ERR_BUSY;
    }

    const auto drivers = registered_drivers();
    if (drivers.empty()) {
        log_fmt(MP_LOG_DEBUG, "no ASIO driver is registered on this machine");
        return MP_ERR_UNSUPPORTED;
    }
    const std::wstring wanted = to_wide(device_id);
    const Registered* chosen = nullptr;
    for (const Registered& d : drivers) {
        if (wanted.empty() || _wcsicmp(d.clsid.c_str(), wanted.c_str()) == 0 ||
            _wcsicmp(d.key.c_str(), wanted.c_str()) == 0) {
            chosen = &d;
            break;
        }
    }
    if (chosen == nullptr) {
        return MP_ERR_INVALID;
    }

    CLSID clsid{};
    if (FAILED(::CLSIDFromString(chosen->clsid.c_str(), &clsid))) {
        return MP_ERR_INVALID;
    }
    if (chosen->dll.empty()) {
        log_fmt(MP_LOG_DEBUG, "%s has no InprocServer32",
                to_utf8(chosen->key.c_str()).c_str());
        return MP_ERR_UNSUPPORTED;
    }

    // **The driver DLL is loaded here rather than by `CoCreateInstance`, and
    // that is a finding rather than a shortcut.**
    //
    // Every ASIO driver is registered `ThreadingModel = Apartment`, and this
    // engine initialises COM as `COINIT_MULTITHREADED` because that is what
    // WASAPI wants. COM's answer to an STA object asked for from an MTA thread
    // is to create it in a single-threaded apartment and hand back a *proxy* --
    // and a proxy needs a marshaller, which an ASIO interface has never had.
    // Measured, and it is the whole of the difficulty: `CoCreateInstance`
    // returned `E_NOINTERFACE` on a driver that was sitting right there.
    //
    // The three ways out are to make the engine STA, which would be choosing a
    // sink's apartment for the whole program; to marshal by hand, which needs
    // the proxy that does not exist; or to skip the apartment machinery, which
    // is what this does. **That is correct rather than a cheat**: an ASIO
    // driver is in-process by definition, its interface is never marshalled,
    // and it runs its own thread regardless of anybody's apartment. Calling
    // `DllGetClassObject` is what `CoCreateInstance` would do for an in-proc
    // server whose apartment already matched.
    HMODULE dll = ::LoadLibraryW(chosen->dll.c_str());
    if (dll == nullptr) {
        log_fmt(MP_LOG_DEBUG, "%s: could not load %s",
                to_utf8(chosen->key.c_str()).c_str(), to_utf8(chosen->dll.c_str()).c_str());
        return MP_ERR_IO;
    }
    using GetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
    auto* get_class_object =
        reinterpret_cast<GetClassObject>(::GetProcAddress(dll, "DllGetClassObject"));
    if (get_class_object == nullptr) {
        ::FreeLibrary(dll);
        return MP_ERR_IO;
    }
    IClassFactory* factory = nullptr;
    HRESULT hr = get_class_object(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        ::FreeLibrary(dll);
        log_fmt(MP_LOG_DEBUG, "%s: DllGetClassObject failed: 0x%08lx",
                to_utf8(chosen->key.c_str()).c_str(), static_cast<unsigned long>(hr));
        return MP_ERR_IO;
    }
    void* raw = nullptr;
    // **The CLSID is the IID too**, which is ASIO's convention and not COM's:
    // an ASIO driver's interface has no separate identifier.
    hr = factory->CreateInstance(nullptr, clsid, &raw);
    factory->Release();
    if (FAILED(hr) || raw == nullptr) {
        ::FreeLibrary(dll);
        log_fmt(MP_LOG_DEBUG, "%s: CreateInstance failed: 0x%08lx",
                to_utf8(chosen->key.c_str()).c_str(), static_cast<unsigned long>(hr));
        return MP_ERR_IO;
    }

    auto* sink = new (std::nothrow) MpSink{};
    if (sink == nullptr) {
        static_cast<IASIO*>(raw)->Release();
        ::FreeLibrary(dll);
        return MP_ERR_NO_MEMORY;
    }
    sink->dll = dll;
    sink->driver = static_cast<IASIO*>(raw);
    sink->clsid = chosen->clsid;
    sink->name = to_utf8((chosen->description.empty() ? chosen->key : chosen->description)
                             .c_str());

    // `init` takes a window handle on Windows, for drivers that want to parent
    // a control panel. There is no window here and no control panel is opened,
    // and every driver tried accepts null for that.
    if (sink->driver->init(nullptr) != ASIOTrue) {
        char why[128] = {};
        sink->driver->getErrorMessage(why);
        log_fmt(MP_LOG_WARN, "%s would not initialise: %s", sink->name.c_str(), why);
        delete sink;
        return MP_ERR_IO;
    }

    long inputs = 0;
    if (sink->driver->getChannels(&inputs, &sink->outputs) != ASE_OK || sink->outputs <= 0) {
        delete sink;
        return MP_ERR_IO;
    }
    sink->free_event = ::CreateEventW(nullptr, FALSE, TRUE, nullptr); // starts free
    if (sink->free_event == nullptr) {
        delete sink;
        return MP_ERR_INTERNAL;
    }
    log_fmt(MP_LOG_DEBUG, "%s: %ld outputs", sink->name.c_str(), sink->outputs);
    *out = sink;
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

void MP_CALL sink_close(MpSink* sink) noexcept
{
    if (sink == nullptr) {
        return;
    }
    MpSink* expected = sink;
    g_open.compare_exchange_strong(expected, nullptr);
    delete sink;
}

MpResult MP_CALL sink_negotiate(MpSink* sink, const MpFormat* want, MpFormat* accepted) noexcept
try {
    if (sink == nullptr || want == nullptr || accepted == nullptr) {
        return MP_ERR_INVALID;
    }
    if (want->channels == 0 || static_cast<long>(want->channels) > sink->outputs) {
        return MP_ERR_FORMAT;
    }
    if (sink->ready) {
        sink->driver->disposeBuffers();
        sink->ready = false;
        g_open.store(nullptr, std::memory_order_release);
    }

    const bool want_dsd = want->encoding == MP_ENCODING_DOP;
    if (want->encoding != MP_ENCODING_PCM && !want_dsd) {
        return MP_ERR_FORMAT;
    }

    // **DSD is a mode the driver is put into, not a format it is offered.**
    // `kAsioSetIoFormat` switches the whole device, so it is asked first and
    // the sample type is read afterwards rather than proposed.
    ASIOIoFormat io{};
    io.FormatType = want_dsd ? kASIODSDFormat : kASIOPCMFormat;
    if (sink->driver->future(kAsioCanDoIoFormat, &io) != ASE_SUCCESS) {
        if (want_dsd) {
            log_fmt(MP_LOG_DEBUG, "%s does not do native DSD", sink->name.c_str());
            return MP_ERR_FORMAT;
        }
        // A driver that does not answer the question at all is a PCM-only
        // driver, which is the overwhelming majority, and PCM is what was
        // asked for. Carry on.
    } else if (sink->driver->future(kAsioSetIoFormat, &io) != ASE_SUCCESS) {
        return MP_ERR_FORMAT;
    }

    // The rate the *device* runs at. For DSD that is sixteen times the DoP
    // frame rate: a DoP frame carries two DSD bytes, which is sixteen DSD
    // samples. DSD64 is 176400 DoP frames and 2822400 DSD samples.
    const double device_rate =
        want_dsd ? static_cast<double>(want->sample_rate) * 16.0
                 : static_cast<double>(want->sample_rate);
    if (sink->driver->canSampleRate(device_rate) != ASE_OK) {
        return MP_ERR_FORMAT;
    }
    if (sink->driver->setSampleRate(device_rate) != ASE_OK) {
        return MP_ERR_FORMAT;
    }

    ASIOChannelInfo info{};
    info.channel = 0;
    info.isInput = ASIOFalse;
    if (sink->driver->getChannelInfo(&info) != ASE_OK) {
        return MP_ERR_IO;
    }
    sink->type = info.type;
    sink->dsd = is_dsd_type(info.type);
    sink->dsd_lsb_first = info.type == ASIOSTDSDInt8LSB1;

    if (want_dsd != sink->dsd) {
        log_fmt(MP_LOG_DEBUG, "%s is in %s and %s was asked for", sink->name.c_str(),
                sink->dsd ? "DSD" : "PCM", want_dsd ? "DSD" : "PCM");
        return MP_ERR_FORMAT;
    }
    if (sink->dsd && info.type == ASIOSTDSDInt8NER8) {
        // Eight *bytes* per sample rather than eight samples per byte. Nothing
        // seen writes it, and guessing at a layout is how channels end up in
        // the wrong speakers.
        log_fmt(MP_LOG_WARN, "%s wants DSDInt8NER8, which this module does not write",
                sink->name.c_str());
        return MP_ERR_FORMAT;
    }
    // **Containers, not enum values.** This tree's own `classify` says it in as
    // many words: a four-byte container holding 24 valid bits is the same wire
    // format whether it is spelled `s24_in_32` or `s32` with `valid_bits = 24`.
    // Comparing the enums instead refused every format the KA5 can play -- it
    // is `Int32LSB`, and a 24-bit source is offered as `S24_IN_32`, and the two
    // are the same four bytes. Measured before this was written: four
    // candidates offered, four refused, on a device that plays all of them.
    //
    // Float is the exception and has to be compared exactly: `Float32LSB` and
    // `Int32LSB` are both four bytes and are not the same bytes.
    if (!sink->dsd) {
        const std::uint32_t theirs = asio_container_bytes(info.type);
        const std::uint32_t ours = host_container_bytes(want->sample_type);
        const bool float_theirs = asio_is_float(info.type);
        const bool float_ours = want->sample_type == MP_SAMPLE_F32;
        if (theirs == 0 || ours == 0 || theirs != ours || float_theirs != float_ours) {
            log_fmt(MP_LOG_DEBUG, "%s is %s and sample type %u was asked for",
                    sink->name.c_str(), type_name(info.type), want->sample_type);
            return MP_ERR_FORMAT;
        }
    }

    long min_size = 0;
    long max_size = 0;
    long preferred = 0;
    long granularity = 0;
    if (sink->driver->getBufferSize(&min_size, &max_size, &preferred, &granularity) != ASE_OK ||
        preferred <= 0) {
        return MP_ERR_IO;
    }
    sink->buffer_frames = preferred;

    sink->buffers.assign(static_cast<std::size_t>(want->channels), ASIOBufferInfo{});
    for (std::uint32_t c = 0; c < want->channels; ++c) {
        sink->buffers[c].isInput = ASIOFalse;
        sink->buffers[c].channelNum = static_cast<long>(c);
    }
    if (sink->driver->createBuffers(sink->buffers.data(), static_cast<long>(want->channels),
                                    sink->buffer_frames, &g_callbacks) != ASE_OK) {
        char why[128] = {};
        sink->driver->getErrorMessage(why);
        log_fmt(MP_LOG_WARN, "%s would not give %ld frames: %s", sink->name.c_str(),
                sink->buffer_frames, why);
        return MP_ERR_FORMAT;
    }

    sink->format = *want;
    if (sink->dsd) {
        // The driver counts in DSD samples and the host counts in DoP frames.
        sink->period_frames = static_cast<std::uint32_t>(sink->buffer_frames) / 16u;
        sink->channel_bytes = static_cast<std::uint32_t>(sink->buffer_frames) / 8u;
        sink->frame_bytes = k_dop_frame_bytes * want->channels;
    } else {
        sink->period_frames = static_cast<std::uint32_t>(sink->buffer_frames);
        const std::uint32_t width = asio_container_bytes(sink->type);
        sink->channel_bytes = sink->period_frames * width;
        sink->frame_bytes = width * want->channels;
    }
    if (sink->period_frames == 0) {
        sink->driver->disposeBuffers();
        return MP_ERR_FORMAT;
    }
    sink->staging.assign(static_cast<std::size_t>(sink->period_frames) * sink->frame_bytes, 0);
    sink->filled.store(0, std::memory_order_release);
    sink->played.store(0, std::memory_order_release);
    sink->starved.store(0, std::memory_order_release);
    ::SetEvent(sink->free_event);
    sink->ready = true;
    g_open.store(sink, std::memory_order_release);

    *accepted = sink->format;
    log_fmt(MP_LOG_INFO, "%s: %s at %.0f Hz, %ld frames a period, %u %s frames to the host",
            sink->name.c_str(), type_name(sink->type), device_rate, sink->buffer_frames,
            sink->period_frames, sink->dsd ? "DoP" : "PCM");
    return MP_OK;
} catch (...) {
    return MP_ERR_NO_MEMORY;
}

MpResult MP_CALL sink_get_period(MpSink* sink, std::uint32_t* frames) noexcept
{
    if (sink == nullptr || frames == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    *frames = sink->period_frames;
    return MP_OK;
}

MpResult MP_CALL sink_start(MpSink* sink) noexcept
{
    if (sink == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    if (sink->started) {
        return MP_OK;
    }
    if (sink->driver->start() != ASE_OK) {
        return MP_ERR_IO;
    }
    sink->started = true;
    return MP_OK;
}

MpResult MP_CALL sink_stop(MpSink* sink) noexcept
{
    if (sink == nullptr || !sink->started) {
        return MP_OK;
    }
    sink->driver->stop();
    sink->started = false;
    // Whatever was waiting is not going to be taken now.
    ::SetEvent(sink->free_event);
    return MP_OK;
}

// MP_RT from here down: no allocation, no logging, no locking.

MpResult MP_CALL sink_wait(MpSink* sink, std::uint32_t timeout_ms) noexcept
{
    const DWORD waited = ::WaitForSingleObject(sink->free_event, timeout_ms);
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
    if (sink->filled.load(std::memory_order_acquire) != 0) {
        // The driver has not taken the last one yet. Nothing to write into,
        // and the host's answer to that is to wait again.
        *ptr = nullptr;
        *frames = 0;
        return MP_OK;
    }
    *ptr = sink->staging.data();
    *frames = sink->period_frames;
    return MP_OK;
}

MpResult MP_CALL sink_commit(MpSink* sink, std::uint32_t frames, std::uint32_t flags) noexcept
{
    if (frames == 0) {
        return MP_OK;
    }
    if (frames > sink->period_frames) {
        return MP_ERR_INVALID;
    }
    if ((flags & MP_COMMIT_SILENT) != 0) {
        std::memset(sink->staging.data(), 0,
                    static_cast<std::size_t>(frames) * sink->frame_bytes);
    }
    sink->filled.store(frames, std::memory_order_release);
    return MP_OK;
}

MpResult MP_CALL sink_get_position(MpSink* sink, std::uint64_t* frames,
                                   std::uint64_t* qpc) noexcept
{
    if (sink == nullptr || frames == nullptr || qpc == nullptr || !sink->ready) {
        return MP_ERR_INVALID;
    }
    ASIOSamples samples{};
    ASIOTimeStamp stamp{};
    if (sink->driver->getSamplePosition(&samples, &stamp) == ASE_OK) {
        // ASIO's 64-bit values are a high and a low word on Windows, because
        // the interface predates a portable 64-bit integer.
        const std::uint64_t device =
            (static_cast<std::uint64_t>(samples.hi) << 32) | samples.lo;
        // In DSD the driver counts DSD samples and the host counts DoP frames.
        *frames = sink->dsd ? device / 16u : device;
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        *qpc = static_cast<std::uint64_t>(now.QuadPart);
        return MP_OK;
    }
    // A driver that will not say. What is known is what was handed over, which
    // is ahead of what was played by at most one period.
    *frames = sink->played.load(std::memory_order_relaxed);
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    *qpc = static_cast<std::uint64_t>(now.QuadPart);
    return MP_OK;
}

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
    // **Below sink_wasapi's 100, on purpose.** WASAPI exclusive is already
    // exact and works on every device; ASIO needs a driver, takes the whole
    // device, and buys nothing for PCM. It is asked for by name.
    /* priority    */ 50,
    /* id          */ "sink_asio",
    /* name        */ "ASIO (exclusive, and native DSD)",
    /* init        */ &module_init,
    /* shutdown    */ &module_shutdown,
    /* vtbl        */ &g_sink_vtbl,
};

} // namespace

extern "C" MP_EXPORT const MpModuleDesc* MP_CALL mp_module_entry(std::uint32_t host_abi_version)
{
    if (host_abi_version != MP_ABI_VERSION) {
        return nullptr;
    }
    return &g_desc;
}

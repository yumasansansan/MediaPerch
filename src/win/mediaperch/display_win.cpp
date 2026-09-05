// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/display_win.hpp"

#include "mediaperch/win_headers.hpp"

#include <dxgi1_2.h>

#include <thread>

namespace mp::win {

namespace {

constexpr wchar_t k_video_class[] = L"MediaPerchVideo";

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(),
                        needed);
    return out;
}

LRESULT CALLBACK video_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_CLOSE || message == WM_DESTROY) {
        auto* closed = reinterpret_cast<bool*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (closed != nullptr) {
            *closed = true;
        }
        if (message == WM_CLOSE) {
            // Not destroyed here: the loop that owns it has a frame in flight
            // and a swap chain on this HWND, and pulling it out from under
            // them is a use-after-free with a nicer name.
            return 0;
        }
    }
    if (message == WM_ERASEBKGND) {
        // The swap chain paints every pixel. Erasing first is a flash of white
        // between frames on a resize.
        return 1;
    }
    return DefWindowProcW(window, message, w, l);
}

} // namespace

std::uint64_t qpc_rate() noexcept
{
    static const std::uint64_t rate = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<std::uint64_t>(frequency.QuadPart);
    }();
    return rate;
}

std::uint64_t qpc_now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

// --------------------------------------------------------------------------
// TickClock
// --------------------------------------------------------------------------

bool TickClock::wait()
{
    if (cancelled_) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds{period_us_});
    return !cancelled_;
}

// --------------------------------------------------------------------------
// VBlankClock
// --------------------------------------------------------------------------

VBlankClock::~VBlankClock()
{
    if (output_ != nullptr) {
        static_cast<IDXGIOutput*>(output_)->Release();
    }
}

std::unique_ptr<VBlankClock> VBlankClock::open(void* window)
{
    HMONITOR monitor = nullptr;
    if (window != nullptr) {
        monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST);
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }

    IDXGIOutput* found = nullptr;
    IDXGIOutput* first = nullptr;
    DXGI_OUTPUT_DESC found_desc{};
    for (UINT a = 0; found == nullptr; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT o = 0;; ++o) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (monitor != nullptr && desc.Monitor == monitor) {
                found = output;
                found_desc = desc;
                break;
            }
            if (first == nullptr) {
                // Kept, in case the window is on a monitor DXGI does not
                // enumerate -- a remote session, or one that moved.
                first = output;
                found_desc = desc;
            } else {
                output->Release();
            }
        }
        adapter->Release();
    }

    if (found == nullptr) {
        found = first;
    } else if (first != nullptr && first != found) {
        first->Release();
    }
    factory->Release();
    if (found == nullptr) {
        return nullptr;
    }

    // `new` rather than make_unique: the constructor is private, because the
    // only way to get one of these is to have found an output.
    std::unique_ptr<VBlankClock> clock{new VBlankClock()};
    clock->output_ = found;

    // What it refreshes at, if the mode says. For the log: a picture paced to
    // 60 Hz on a 144 Hz display is right and looks wrong, and the first
    // question is always which one it was.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(found_desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != 0 &&
        mode.dmDisplayFrequency > 1) {
        clock->refresh_hz_ = static_cast<double>(mode.dmDisplayFrequency);
    }
    return clock;
}

bool VBlankClock::wait()
{
    if (cancelled_ || output_ == nullptr) {
        return false;
    }
    // **This blocks a whole refresh.** That is what it is for, and it is why
    // the loop that calls it must not be the one pumping the window's message
    // queue -- a queue nobody drains for 16 ms is a window Windows calls
    // unresponsive.
    if (FAILED(static_cast<IDXGIOutput*>(output_)->WaitForVBlank())) {
        return false;
    }
    return !cancelled_;
}

// --------------------------------------------------------------------------
// WallClock
// --------------------------------------------------------------------------

WallClock::WallClock(std::uint32_t rate) noexcept
{
    spec_.wire_rate = rate;
    spec_.source_rate = rate;
    // Filled by the loop from its frame clock, which is the same counter this
    // stamps its readings with.
    spec_.tick_rate = 0;
}

void WallClock::start() noexcept
{
    origin_ = qpc_now();
    running_ = true;
}

bool WallClock::read(ClockReading& out)
{
    if (!running_) {
        return false;
    }
    const std::uint64_t now = qpc_now();
    const std::uint64_t elapsed = now >= origin_ ? now - origin_ : 0;
    // Frames at the nominal rate, which is what a device would have played by
    // now if there were one.
    // One multiply before the divide, so a tick is not rounded away: an hour
    // at ten megahertz times forty-eight thousand is 1.7e17, well inside
    // sixty-four bits.
    out.device_frames = elapsed * spec_.wire_rate / qpc_rate();
    out.ticks = now;
    return true;
}

// --------------------------------------------------------------------------
// VideoWindow
// --------------------------------------------------------------------------

VideoWindow::~VideoWindow()
{
    close();
}

bool VideoWindow::open(const std::string& title, std::uint32_t width, std::uint32_t height,
                       std::string& why)
{
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &video_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = k_video_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // A class that is already registered is not an error, for the reason the
    // tray gives: two of these in one process is a test rather than a mistake.
    RegisterClassExW(&cls);

    // The client area is the picture, so the frame is added to it rather than
    // taken out of it -- a window sized to the picture shows the picture at its
    // own size, which is what "no scaling" has to mean before anything else.
    RECT wanted{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&wanted, style, FALSE);

    const HWND window = CreateWindowExW(
        0, k_video_class, widen(title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
        wanted.right - wanted.left, wanted.bottom - wanted.top, nullptr, nullptr,
        cls.hInstance, nullptr);
    if (window == nullptr) {
        why = "could not create a window";
        return false;
    }
    closed_ = false;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&closed_));
    window_ = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

bool VideoWindow::pump_messages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !closed_;
}

void VideoWindow::close() noexcept
{
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
    closed_ = true;
}

} // namespace mp::win

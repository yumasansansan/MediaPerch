// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediaperch/tray.hpp"

#include "mediaperch/platform.hpp"
#include "mediaperch/win_headers.hpp"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string_view>

namespace mp::win {
namespace {

constexpr UINT k_callback = WM_APP + 1;
constexpr UINT k_icon_id = 1;
constexpr UINT_PTR k_timer = 1;

enum Command : unsigned {
    cmd_play_pause = 100,
    cmd_next,
    cmd_previous,
    cmd_stop,
    cmd_settings,
    cmd_exit,
};

const wchar_t* k_class = L"MediaPerchTray";

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(),
                        needed);
    return out;
}

/// Into the fixed tip buffer, cut to fit. `lstrcpynW` did this and the
/// analyzer wants its result read, which is a null nobody can act on.
void put_tip(NOTIFYICONDATAW& icon, std::wstring_view tip) noexcept
{
    const std::size_t room = std::size(icon.szTip) - 1;
    const std::size_t n = tip.size() < room ? tip.size() : room;
    std::copy_n(tip.data(), n, icon.szTip);
    icon.szTip[n] = L'\0';
}

/// The last component of a path, which is what a tooltip has room for.
std::string leaf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::string installed_shell()
{
    std::error_code ec;
    const auto shell = module_directory() / "mediaperch-shell.exe";
    return std::filesystem::exists(shell, ec) ? shell.string() : std::string{};
}

Tray::~Tray()
{
    hide();
}

long long __stdcall Tray::proc(void* window, unsigned message, unsigned long long w,
                               long long l)
{
    const HWND hwnd = static_cast<HWND>(window);
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        switch (message) {
        case k_callback:
            // Either button opens the menu; a double click is play/pause,
            // because that is what every other tray icon in the world does.
            if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP ||
                LOWORD(l) == WM_CONTEXTMENU) {
                self->on_menu();
            } else if (LOWORD(l) == WM_LBUTTONDBLCLK) {
                self->on_command(cmd_play_pause);
            }
            return 0;
        case WM_COMMAND:
            self->on_command(LOWORD(w));
            return 0;
        case WM_TIMER:
            self->refresh();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, message, static_cast<WPARAM>(w), static_cast<LPARAM>(l));
}

bool Tray::show(std::string& why)
{
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = reinterpret_cast<WNDPROC>(&Tray::proc);
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = k_class;
    // A class that is already registered is not an error: two engines in one
    // process is a test, not a mistake.
    RegisterClassExW(&cls);

    // Never shown. It exists to receive the icon's messages, which is the only
    // way Windows will deliver them.
    const HWND window = CreateWindowExW(0, k_class, L"MediaPerch", 0, 0, 0, 0, 0, nullptr,
                                        nullptr, cls.hInstance, nullptr);
    if (window == nullptr) {
        why = "could not create the window the notification icon needs";
        return false;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    window_ = window;

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = k_icon_id;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = k_callback;
    // The stock application icon: an engine with no artwork on disk is still
    // an engine, and a missing icon file would be a worse failure than a plain
    // one.
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    put_tip(icon, L"MediaPerch");
    if (Shell_NotifyIconW(NIM_ADD, &icon) == FALSE) {
        // A service, or a session with no Explorer. Not an error: the engine
        // does not need a desktop and never did.
        why = "there is no notification area to put an icon in";
        DestroyWindow(window);
        window_ = nullptr;
        return false;
    }
    visible_ = true;
    SetTimer(window, k_timer, 250, nullptr);
    return true;
}

void Tray::hide()
{
    if (visible_) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = static_cast<HWND>(window_);
        icon.uID = k_icon_id;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        visible_ = false;
    }
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
}

void Tray::refresh()
{
    if (!visible_) {
        return;
    }
    const ipc::Status status = player_->status();
    std::string tip = "MediaPerch";
    if (status.state != ipc::State::stopped && !status.track.empty()) {
        tip += status.state == ipc::State::paused ? " -- paused\n" : "\n";
        tip += leaf(status.track);
    }
    if (tip == tip_) {
        return;
    }
    tip_ = tip;

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = static_cast<HWND>(window_);
    icon.uID = k_icon_id;
    icon.uFlags = NIF_TIP;
    put_tip(icon, widen(tip));
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void Tray::on_menu()
{
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const ipc::Status status = player_->status();
    const bool playing = status.state == ipc::State::playing;
    const bool anything = status.state != ipc::State::stopped;

    AppendMenuW(menu, MF_STRING | (anything ? 0u : MF_GRAYED), cmd_play_pause,
                playing ? L"Pause" : L"Play");
    AppendMenuW(menu, MF_STRING | (anything ? 0u : MF_GRAYED), cmd_previous, L"Previous");
    AppendMenuW(menu, MF_STRING | (anything ? 0u : MF_GRAYED), cmd_next, L"Next");
    AppendMenuW(menu, MF_STRING | (anything ? 0u : MF_GRAYED), cmd_stop, L"Stop");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Greyed out when there is nothing to open. An item that silently does
    // nothing looks like a fault in the engine; this one says what is missing.
    const bool have_shell = !installed_shell().empty();
    AppendMenuW(menu, MF_STRING | (have_shell ? 0u : MF_GRAYED), cmd_settings,
                have_shell ? L"Settings..." : L"Settings... (no shell installed)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, cmd_exit, L"Exit");

    POINT where{};
    GetCursorPos(&where);
    const HWND window = static_cast<HWND>(window_);
    // Documented and load-bearing: without this the menu does not close when
    // somebody clicks away from it.
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, where.x, where.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Tray::on_command(unsigned id)
{
    switch (id) {
    case cmd_play_pause:
        if (player_->status().state == ipc::State::playing) {
            player_->pause();
        } else {
            player_->resume();
        }
        break;
    case cmd_next:
        player_->next();
        break;
    case cmd_previous:
        player_->previous();
        break;
    case cmd_stop:
        player_->stop();
        break;
    case cmd_settings: {
        const std::string shell = installed_shell();
        if (shell.empty()) {
            break;
        }
        log_->add("opening " + shell);
        ShellExecuteW(nullptr, L"open", widen(shell).c_str(), nullptr, nullptr, SW_SHOW);
        break;
    }
    case cmd_exit:
        log_->add("the tray menu asked the engine to stop");
        PostQuitMessage(0);
        break;
    default:
        break;
    }
}

void Tray::run(const std::function<bool()>& keep_going)
{
    MSG message{};
    for (;;) {
        // The timer wakes this four times a second, which is how `keep_going`
        // gets asked without a poll of its own.
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            return; // WM_QUIT, or a window that has gone
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (!keep_going()) {
            return;
        }
    }
}

} // namespace mp::win

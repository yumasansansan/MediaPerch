// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's own icon, for an install with no shell on disk.
//
// **This is a fallback, not a user interface.** The shell is a separate process
// and may not exist; an engine that was headless in that case would be a program
// somebody has to open a terminal to pause. So the engine keeps a notification
// icon of its own: play and pause, next and previous, stop, and a way out.
//
// **"Settings" is greyed out when there is no shell to open**, which is the
// honest behaviour. A menu item that does nothing is worse than one that says it
// cannot: the first looks like a bug in the engine and the second tells you what
// to install.
//
// Nothing here touches the audio path. It runs on the process's first thread,
// which otherwise has nothing to do, and it talks to `mp::Player` through the
// same commands a shell uses.

#ifndef MEDIAPERCH_WIN_TRAY_HPP
#define MEDIAPERCH_WIN_TRAY_HPP

#include "mediaperch/log.hpp"
#include "mediaperch/player.hpp"

#include <functional>
#include <string>

namespace mp::win {

class Tray {
public:
    Tray(Player& player, LogRing& log) : player_(&player), log_(&log) {}
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    Tray(Tray&&) = delete;
    Tray& operator=(Tray&&) = delete;

    /// Puts the icon in the notification area. False and a reason when there is
    /// nowhere to put one -- a service, a session with no Explorer -- which is
    /// not an error and not a reason to stop.
    [[nodiscard]] bool show(std::string& why);
    void hide();

    /// Pumps messages until `keep_going()` says otherwise or somebody chooses
    /// Exit. This is the engine's main loop when there is a tray; without one
    /// the main thread waits instead.
    void run(const std::function<bool()>& keep_going);

private:
    static long long __stdcall proc(void* window, unsigned message, unsigned long long w,
                                    long long l);
    void on_command(unsigned id);
    void on_menu();
    void refresh();

    Player* player_;
    LogRing* log_;
    void* window_ = nullptr; // HWND
    bool visible_ = false;
    /// What the tooltip last said, so it is not rewritten four times a second
    /// for no reason.
    std::string tip_;
};

/// Where a shell would be if one were installed: `mediaperch-shell.exe` beside
/// the engine. Empty when there is not one.
[[nodiscard]] std::string installed_shell();

} // namespace mp::win

#endif // MEDIAPERCH_WIN_TRAY_HPP

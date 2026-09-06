// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a display's modes, and putting one in force.
//
// **Two APIs, because neither does both.** DXGI's `GetDisplayModeList` lists
// modes that are not currently active and states each refresh as a
// `DXGI_RATIONAL`; it cannot switch one on without exclusive fullscreen. The
// Connecting and Configuring Displays API can switch, states a refresh as a
// `DISPLAYCONFIG_RATIONAL`, and reports back what is actually in force -- but
// `QueryDisplayConfig` gives a rate only for paths that are already active. So:
// enumerate with the first, validate and set and verify with the second.
//
// **`ChangeDisplaySettingsEx` is not used and the reason is worth keeping.**
// `DEVMODE::dmDisplayFrequency` is a whole number of hertz. On this tree's
// panel that happens to separate 47.952 from 48.000, because Windows truncates
// and the two land on 47 and 48 -- but that is a property of these nine modes
// and not a rule. A display offering 60.000 and 60.003 hands both to
// `EnumDisplaySettings` as 60, and there is nothing in a DEVMODE to say which
// was meant. A whole number of hertz cannot name a refresh rate.
//
// Measured, each row an apply and a read-back through `SetDisplayConfig`:
// asking 48000/1001 puts a 115.20 MHz pixel clock in force and asking 48/1
// puts 112.32 MHz, which are the two EDID timings to the hertz.

#ifndef MEDIAPERCH_REFRESH_WIN_HPP
#define MEDIAPERCH_REFRESH_WIN_HPP

#include "mediaperch/refresh.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mp::win {

/// Every mode the output containing `window` offers, refresh rates exact.
///
/// `window` is an HWND as a `void*`, which is what `VideoWindow::handle` hands
/// out and what `VBlankClock::open` takes: the engine's headers do not include
/// <windows.h> and this one is a Windows header only in where it lives.
///
/// The output that contains the window rather than the adapter's first, for the
/// reason `VBlankClock` finds the same one: on two monitors at different rates
/// they are different lists.
[[nodiscard]] std::vector<DisplayMode> display_modes(void* window, std::string& why);

/// The mode in force on the display `window` is on, as the OS states it.
[[nodiscard]] std::optional<DisplayMode> current_mode(void* window, std::string& why);

/// Puts a refresh rate in force on the display `window` is on, and restores it.
///
/// **The switch changes somebody's desktop**, which is why this is an object
/// with a destructor rather than a function: whatever a run does afterwards,
/// including throwing, the mode goes back. `apply` asks the OS to validate
/// before it applies, so a mode that would not have worked is a refusal rather
/// than a black screen.
class RefreshSwitch final {
public:
    RefreshSwitch() = default;
    ~RefreshSwitch();

    RefreshSwitch(const RefreshSwitch&) = delete;
    RefreshSwitch& operator=(const RefreshSwitch&) = delete;
    RefreshSwitch(RefreshSwitch&&) = delete;
    RefreshSwitch& operator=(RefreshSwitch&&) = delete;

    /// False, with `why` set, when the OS would not take it. Nothing has
    /// changed in that case.
    bool apply(void* window, Rational refresh, std::string& why);

    /// What is in force now, read back rather than assumed. Empty until
    /// `apply` succeeds.
    [[nodiscard]] Rational in_force() const noexcept { return in_force_; }

    /// Puts back what was there. Called by the destructor; safe twice.
    void restore() noexcept;

private:
    bool held_ = false;
    Rational was_;
    Rational in_force_;
    void* window_ = nullptr;
};

} // namespace mp::win

#endif // MEDIAPERCH_REFRESH_WIN_HPP

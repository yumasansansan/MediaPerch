// SPDX-License-Identifier: GPL-3.0-or-later

using Microsoft.UI.Xaml;

namespace MediaPerch.Shell;

/// <summary>
/// The shell's entry point, and almost nothing else.
/// </summary>
/// <remarks>
/// <b>It holds no playback state.</b> §10: the engine is a separate process and
/// this one is killable at any moment without the audio noticing, so everything
/// worth keeping is over the pipe. What lives here is a window.
/// </remarks>
public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}

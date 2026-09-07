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
        // **Written down, whatever it was.** A XAML exception ends the process
        // as a stowed exception with no message anywhere a person looks; this
        // is the one place it can still be named.
        UnhandledException += (_, e) =>
        {
            Ipc.Session.Log("unhandled: " + e.Message + Environment.NewLine + e.Exception);
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            Ipc.Session.Log("unobserved: " + e.Exception);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Ipc.Session.Log("shell: launched");
        _window = new MainWindow();
        _window.Activate();
        Ipc.Session.Log("shell: window up");
    }
}

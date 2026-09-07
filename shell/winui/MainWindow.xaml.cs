// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using MediaPerch.Shell.Pages;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaPerch.Shell;

/// <summary>
/// The window: acrylic behind everything, the title bar extended into, a
/// navigation pane, and a page.
/// </summary>
/// <remarks>
/// <para>
/// <b>Three pages, because the picture wants the whole window.</b> What is
/// playing fills it, with the transport underneath and nothing else in the way;
/// the playlist and the engine's shape -- the node canvas and every setting --
/// are pages of their own. That is the shape Windows' own media player takes,
/// and the reason is the same: a video window is looked at, and a settings
/// canvas is worked in, and the two do not want each other's space.
/// </para>
/// <para>
/// Everything the pages share -- the pipe, the tick, the last status -- is
/// <see cref="Session"/>, so a page holds nothing the next one would have to
/// ask for again.
/// </para>
/// </remarks>
public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        Title = "MediaPerch";
        // **Into the title bar**, and the grid at the top is what a person
        // drags by. The caption buttons stay the system's.
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBar);

        Session.Current.ConnectionChanged += ShowConnection;
        Session.Current.Start(DispatcherQueue);
        ShowConnection();
        // **The engine this window started goes with it.** One that was
        // already running is somebody else's and stays; `Session.Stop` says.
        Closed += (_, _) => Session.Current.Stop();

        Nav.SelectedItem = NowItem;
    }

    private void ShowConnection()
    {
        if (Session.Current.Connected)
        {
            ConnectionLine.Text = string.Empty;
            return;
        }
        string note = Session.Current.EngineNote;
        ConnectionLine.Text = note.Length == 0
            ? "not connected: mediaperchd is not running, or is listening somewhere else"
            : note;
    }

    private void OnNavigate(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItemContainer?.Tag is not string tag)
        {
            return;
        }
        Type? page = tag switch
        {
            "now" => typeof(NowPlayingPage),
            "playlist" => typeof(PlaylistPage),
            "graph" => typeof(GraphPage),
            _ => null,
        };
        if (page is not null && Pages.CurrentSourcePageType != page)
        {
            Pages.Navigate(page);
        }
    }

    private async void OnInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        if (args.InvokedItemContainer?.Tag is string tag && tag == "reconnect")
        {
            await Session.Current.ReconnectAsync();
            ShowConnection();
        }
    }
}

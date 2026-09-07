// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using MediaPerch.Shell.Pages;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Windows.UI.Core;

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

    /// <summary>
    /// The keys every player answers, on every page: space plays or pauses,
    /// left and right go ten seconds, with Ctrl they go a track, Ctrl+O opens
    /// files, and the keyboard's own media keys do what they say.
    /// </summary>
    /// <remarks>
    /// Tunnelled (<c>PreviewKeyDown</c>) so the page underneath need not know,
    /// and stepped around where a control owns the key: a box being typed in
    /// keeps its letters, a focused button keeps its space, and the scrubber
    /// keeps its arrows, which seek through its own event.
    /// </remarks>
    private async void OnKey(object sender, KeyRoutedEventArgs e)
    {
        object? focused = Root.XamlRoot is null ? null : FocusManager.GetFocusedElement(Root.XamlRoot);
        bool typing = focused is TextBox or ComboBox or AutoSuggestBox or PasswordBox;
        bool onSlider = focused is Slider;
        bool ctrl = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
                                       .HasFlag(CoreVirtualKeyStates.Down);
        Session s = Session.Current;
        bool playing = s.Status.State == PlayState.Playing;

        // The media keys are not in `VirtualKey`; these are their codes.
        switch ((int)e.Key)
        {
            case 0xB3:
                e.Handled = true;
                await s.TransportAsync(playing ? Kind.Pause : Kind.Resume);
                return;
            case 0xB0:
                e.Handled = true;
                await s.TransportAsync(Kind.Next);
                return;
            case 0xB1:
                e.Handled = true;
                await s.TransportAsync(Kind.Previous);
                return;
            case 0xB2:
                e.Handled = true;
                await s.TransportAsync(Kind.Stop);
                return;
        }
        if (typing)
        {
            return;
        }
        switch (e.Key)
        {
            case VirtualKey.Space when focused is not Button:
                e.Handled = true;
                await s.TransportAsync(playing ? Kind.Pause : Kind.Resume);
                break;
            case VirtualKey.Left when ctrl:
                e.Handled = true;
                await s.TransportAsync(Kind.Previous);
                break;
            case VirtualKey.Right when ctrl:
                e.Handled = true;
                await s.TransportAsync(Kind.Next);
                break;
            case VirtualKey.Left when !onSlider:
                e.Handled = true;
                await SeekSecondsAsync(-10);
                break;
            case VirtualKey.Right when !onSlider:
                e.Handled = true;
                await SeekSecondsAsync(10);
                break;
            case VirtualKey.O when ctrl:
                e.Handled = true;
                await s.PlayFilesAsync(await Controls.FilePicking.PickAsync(), replace: true);
                break;
        }
    }

    private static async Task SeekSecondsAsync(int seconds)
    {
        uint rate = Session.Current.Status.Source.SampleRate;
        if (rate != 0)
        {
            await Session.Current.SeekAsync((long)seconds * rate, true);
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

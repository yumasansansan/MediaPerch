// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace MediaPerch.Shell.Pages;

/// <summary>
/// Every track, with an arrow at the one being heard, and a click to play one.
/// </summary>
public sealed partial class PlaylistPage : Page
{
    private readonly List<string> _rows = new();

    public PlaylistPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        Session.Current.Changed += Show;
        Show();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        Session.Current.Changed -= Show;
    }

    private void Show()
    {
        IReadOnlyList<string> files = Session.Current.Playlist;
        uint at = Session.Current.PlaylistAt;
        bool playing = Session.Current.Status.State != PlayState.Stopped;
        var drawn = new List<string>(files.Count);
        for (int i = 0; i < files.Count; ++i)
        {
            string mark = playing && i == at ? "▶" : " ";
            drawn.Add($"{mark}  {i + 1,3}.  {Session.Leaf(files[i])}");
        }
        Heading.Text = files.Count == 0 ? "Playlist" : $"Playlist  ({files.Count})";
        if (drawn.SequenceEqual(_rows))
        {
            return;
        }
        _rows.Clear();
        _rows.AddRange(drawn);
        // Rebinding rather than mutating: `List<T>` says nothing when it
        // changes, and a list that quietly did not update would be worse than
        // one that flickers.
        Rows.ItemsSource = null;
        Rows.ItemsSource = _rows;
    }

    private async void OnOpen(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        Tell(await Session.Current.PlayFilesAsync(await Controls.FilePicking.PickAsync(), true));

    private async void OnAdd(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        Tell(await Session.Current.PlayFilesAsync(await Controls.FilePicking.PickAsync(), false));

    private async void OnClear(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        Tell(await Session.Current.ClearAsync());

    private void Tell(string why)
    {
        Note.IsOpen = why.Length != 0;
        Note.Message = why;
    }

    private async void OnItemClick(object sender, ItemClickEventArgs e)
    {
        int index = _rows.IndexOf((string)e.ClickedItem);
        if (index < 0)
        {
            return;
        }
        string why = await Session.Current.PlayAtAsync((uint)index);
        Note.IsOpen = why.Length != 0;
        Note.Message = why;
    }
}

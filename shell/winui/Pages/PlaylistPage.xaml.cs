// SPDX-License-Identifier: GPL-3.0-or-later

using System.Collections.ObjectModel;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace MediaPerch.Shell.Pages;

/// <summary>
/// Every track, with an arrow at the one being heard, and a click to play one.
/// </summary>
public sealed partial class PlaylistPage : Page
{
    /// Observable, because a list that reorders its own items needs a
    /// collection that says so; bound once and changed in place.
    private readonly ObservableCollection<string> _rows = new();
    /// Where a drag began, and whether one is under way -- the tick must not
    /// redraw the list from under a person's pointer.
    private int _dragFrom = -1;
    private bool _dragging;

    public PlaylistPage()
    {
        InitializeComponent();
        Rows.ItemsSource = _rows;
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
        if (_dragging || drawn.SequenceEqual(_rows))
        {
            return;
        }
        _rows.Clear();
        foreach (string row in drawn)
        {
            _rows.Add(row);
        }
    }

    private void OnDragStarting(object sender, DragItemsStartingEventArgs e)
    {
        _dragging = true;
        _dragFrom = e.Items.Count == 1 && e.Items[0] is string row ? _rows.IndexOf(row) : -1;
    }

    private async void OnDragCompleted(ListViewBase sender, DragItemsCompletedEventArgs args)
    {
        _dragging = false;
        int from = _dragFrom;
        _dragFrom = -1;
        int to = args.Items.Count == 1 && args.Items[0] is string row ? _rows.IndexOf(row) : -1;
        if (from < 0 || to < 0 || from == to)
        {
            Show();
            return;
        }
        // The list has already moved on the screen; the engine is asked to do
        // the same, and what it says goes -- a refusal puts the list back.
        Tell(await Session.Current.MoveAsync((uint)from, (uint)to));
        Show();
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

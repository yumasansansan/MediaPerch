// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaPerch.Shell;

/// <summary>
/// The first window: connect, ask what is playing, ask what shape the engine is
/// in, and draw both.
/// </summary>
/// <remarks>
/// <para>
/// <b>This is the wire being proved, not the shell being designed.</b> The node
/// canvas §10 describes is what goes where the list is; putting the same answer
/// on the screen as text first means the framing, the ids and the field order
/// are checked against a running engine before anything is drawn on top of them.
/// </para>
/// <para>
/// Every call may come back null, and that is the ordinary case rather than an
/// error: the engine is a separate process that may not be running, and §10 says
/// a shell that cannot reach it draws that state.
/// </para>
/// </remarks>
public sealed partial class MainWindow : Window
{
    private readonly EngineClient _engine = new();
    private readonly List<string> _nodes = new();

    public MainWindow()
    {
        InitializeComponent();
        Title = "MediaPerch";
        Nodes.ItemsSource = _nodes;
        _ = ReconnectAndRefreshAsync();
    }

    private async Task ReconnectAndRefreshAsync()
    {
        if (!_engine.Connected)
        {
            // A second is long enough for a service that is running and short
            // enough that a window does not sit blank while one is not.
            await _engine.ConnectAsync(1000, CancellationToken.None);
        }
        if (!_engine.Connected)
        {
            Connection.Severity = InfoBarSeverity.Informational;
            Connection.Title = "Not connected";
            Connection.Message =
                "mediaperchd is not running, or is listening somewhere else.";
            return;
        }

        Connection.Severity = InfoBarSeverity.Success;
        Connection.Title = "Connected";
        Connection.Message = "The engine is on the pipe.";

        await RefreshStatusAsync();
        await RefreshGraphAsync();
    }

    private async Task RefreshStatusAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Status);
        if (answer is null || !answer.Value.Is(Kind.StatusReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        Status status = Decode.ReadStatus(r);
        if (!r.Complete)
        {
            // A reply this build cannot read is a version mismatch, and saying
            // so beats drawing half of it.
            Connection.Severity = InfoBarSeverity.Warning;
            Connection.Title = "The engine speaks a wire this shell does not";
            return;
        }

        TrackLine.Text = status.State == PlayState.Stopped || status.Track.Length == 0
            ? "Nothing is playing"
            : status.Track;
        FormatLine.Text = status.Wire.SampleRate == 0
            ? string.Empty
            : $"{status.Wire.SampleRate} Hz, {status.Wire.Channels} ch"
              + $"{(status.Processed ? ", processed" : ", bit-exact")}"
              + $"  —  {status.Device}"
              + (status.Underruns == 0 ? string.Empty : $", {status.Underruns} underruns");
        PlayPause.Content = status.State == PlayState.Playing ? "Pause" : "Resume";
    }

    private async Task RefreshGraphAsync()
    {
        _nodes.Clear();
        Answer? answer = await _engine.CallAsync(Kind.Graph);
        if (answer is not null && answer.Value.Is(Kind.GraphReply))
        {
            Reader r = answer.Value.Reader();
            Graph graph = Decode.ReadGraph(r);
            foreach (Node node in graph.Nodes)
            {
                string module = node.Module.Length == 0 ? string.Empty : $"  [{node.Module}]";
                _nodes.Add($"{node.Id,-12} {node.Kind,-12} {node.Name}{module}");
            }
            foreach (Edge edge in graph.Edges)
            {
                _nodes.Add($"             {edge.From} → {edge.To}");
            }
        }
        // Rebinding rather than mutating, because `List<T>` says nothing when it
        // changes and a canvas that quietly did not update would be worse than
        // one that flickers.
        Nodes.ItemsSource = null;
        Nodes.ItemsSource = _nodes;
    }

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await ReconnectAndRefreshAsync();
    }

    private async void OnPlayPause(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(PlayPause.Content as string == "Pause" ? Kind.Pause
                                                                      : Kind.Resume);
        await RefreshStatusAsync();
    }

    private async void OnStop(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(Kind.Stop);
        await RefreshStatusAsync();
    }
}
